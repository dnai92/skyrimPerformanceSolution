#include "Occlusion.h"

#include "Config.h"
#include "OcclusionGpu.h"

#include <memory>

namespace Occlusion
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// Kamera eines Frames, wie sie beim Tiefenvorpass galt. Die Projektion wird beim Merken selbst geprueft:
		// Matrix-Konvention (Zeilen-/Spaltenvektor) und ob Positionen relativ zu posAdjust erwartet werden.
		struct CamSnap
		{
			float         m[4][4]{};
			bool          colVec = true;     // clip = M * v (sonst v * M)
			bool          relative = true;   // v = Welt - posAdjust
			RE::NiPoint3  posAdjust{};
			float         depthA = 0.0f, depthB = 0.0f;  // Tiefe(NDC) = A + B / w
			bool          reversed = false;              // fern = kleiner Wert
			float         sx = 1.0f, sy = 1.0f;          // Projektions-Skalierung x/y
			std::uint64_t frame = 0;
			bool          valid = false;
		};

		// Eigene Projektion aus der Spielkamera (NiCamera): die Renderer-Matrix am Ende des Tiefenvorpasses gehoerte im
		// Test nur in ~10 % der Frames zur Hauptkamera. Vorzeichen der Achsen und die Tiefenformel werden an den Frames
		// abgeglichen, in denen die Renderer-Matrix passt; sonst Standard-Z aus near/far des Frustums.
		struct OwnCam
		{
			RE::NiPoint3  pos{}, dir{}, up{}, right{};
			float         l = -1.0f, r = 1.0f, t = 1.0f, b = -1.0f;
			float         depthA = 0.0f, depthB = 0.0f;  // Tiefe(NDC) = A + B / z
			bool          reversed = false;
			std::uint64_t frame = 0;
			bool          valid = false;
			// GPU-Matrix vom Beginn des Tiefenvorpasses (damit entsteht das Tiefenbild) - nur dann wird weggelassen
			bool          gpu = false;
			CamSnap       gm{};
			float         sx = 1.0f, sy = 1.0f;  // Projektions-Skalierung x/y der GPU-Matrix
			// Viewport-Tiefenbereich: die Hauptszene liegt nur in [depthMin, depthMax] des Puffers, darueber der Himmel.
			// Ohne Umrechnung endeten alle Entfernungen bei ~450 Einheiten -> ferne sichtbare Objekte galten als verdeckt.
			float         depthMin = 0.0f, depthMax = 1.0f;
		};

		float         g_rightSign = 1.0f, g_upSign = 1.0f;
		bool          g_learned = false;
		float         g_learnA = 0.0f, g_learnB = 0.0f;
		bool          g_learnRev = false;
		std::uint64_t g_learnFrames = 0;
		bool          g_signsVerified = false;  // Achsen einmal an der GPU-Matrix bestaetigt - vorher wird nichts weggelassen
		std::uint64_t g_gpuSignFrames = 0;
		float         g_learnMaxErr = 0.0f;  // groesste Abweichung eigene vs. Renderer-Projektion (NDC) im Bericht

		float Dot(const RE::NiPoint3& a_a, const RE::NiPoint3& a_b) noexcept { return a_a.x * a_b.x + a_a.y * a_b.y + a_a.z * a_b.z; }

		// NDC x/y und Sichttiefe z
		void Project(const OwnCam& a_c, const RE::NiPoint3& a_p, float& a_x, float& a_y, float& a_z) noexcept
		{
			const RE::NiPoint3 v{ a_p.x - a_c.pos.x, a_p.y - a_c.pos.y, a_p.z - a_c.pos.z };
			a_z = Dot(v, a_c.dir);
			const float iz = a_z != 0.0f ? 1.0f / a_z : 0.0f;
			a_x = (Dot(v, a_c.right) * iz - a_c.l) / (a_c.r - a_c.l) * 2.0f - 1.0f;
			a_y = (Dot(v, a_c.up) * iz - a_c.b) / (a_c.t - a_c.b) * 2.0f - 1.0f;
		}

		// Tiefenbild + zugehoerige Kamera, unveraenderlich nach dem Veroeffentlichen. GetRenderPasses laeuft auf
		// Worker-Threads (1.1.0-Test: ~1800 Objekte/Frame, keines im Main-Thread) -> Austausch nur ueber shared_ptr.
		struct Snapshot
		{
			OcclusionGpu::Readback rb;
			OwnCam                 cam;
			std::uint32_t          id = 0;  // fortlaufend je veroeffentlichtem Tiefenbild
		};
		std::atomic<std::shared_ptr<const Snapshot>> g_current;
		std::uint32_t                                g_nextSnapshotId = 1;  // Main-Thread

		// Stufe 2 (Weglassen): nur im Accumulator des Hauptbilds (der mit den meisten Objekten je Sekunde), nur feste
		// Objekte, erst nach zwei Tiefenbildern hintereinander verdeckt, nicht bei schneller Kamerabewegung.
		std::atomic<const void*> g_mainAcc{ nullptr };
		std::atomic<bool>        g_cullAllowed{ false };  // Kamera seit dem Tiefenbild kaum bewegt (Main-Thread setzt)
		struct Streak
		{
			std::uint32_t lastId = 0;  // Tiefenbild der letzten Pruefung
			std::uint8_t  count = 0;   // so viele Tiefenbilder hintereinander verdeckt
		};
		struct Shard
		{
			std::mutex                                     lock;
			std::unordered_map<const void*, Streak>        map;
		};
		constexpr std::size_t kShards = 32;
		Shard                 g_shards[kShards];

		// true = in den letzten zwei Tiefenbildern verdeckt
		bool UpdateStreak(const void* a_geom, std::uint32_t a_snapId, bool a_occluded) noexcept
		{
			auto&            sh = g_shards[(reinterpret_cast<std::uintptr_t>(a_geom) >> 4) % kShards];
			std::scoped_lock l(sh.lock);
			auto&            e = sh.map[a_geom];
			const auto       need = Config::occlStreak.load(std::memory_order_relaxed);
			if (e.lastId == a_snapId) {
				return a_occluded && e.count >= need;  // mehrere Passes desselben Objekts im selben Frame
			}
			if (!a_occluded) {
				e.count = 0;
			} else {
				e.count = (e.lastId + 1 == a_snapId) ? static_cast<std::uint8_t>(std::min(e.count + 1, 8)) : 1;
			}
			e.lastId = a_snapId;
			return e.count >= need;
		}

		void ClearStreaks() noexcept
		{
			for (auto& sh : g_shards) {
				std::scoped_lock l(sh.lock);
				sh.map.clear();
			}
		}

		// Main-Thread
		constexpr std::uint32_t kSnaps = 8;
		OwnCam        g_snaps[kSnaps];
		std::uint32_t g_nextTag = 0;
		bool          g_snapPending = false;  // Kamera dieses Frames gemerkt, Tiefe noch nicht eingereicht
		std::uint64_t g_frame = 0;
		Clock::time_point g_reportStart = Clock::now();
		bool          g_initTried = false;
		std::uint32_t g_lastSource = 0;
		std::uint32_t g_lastSrcW = 0, g_lastSrcH = 0;

		// Minutenbericht (Zaehler von allen Threads)
		struct AccStats
		{
			std::atomic<const void*>   acc{ nullptr };
			std::atomic<std::uint64_t> geoms{ 0 }, draws{ 0 }, tested{ 0 }, occGeoms{ 0 }, occDraws{ 0 }, tooBig{ 0 };
			std::atomic<std::uint64_t> occSkinned{ 0 }, occSmall{ 0 }, occMid{ 0 }, occLarge{ 0 };  // verdeckte Draws nach Radius <50 / <200 / groesser
			std::atomic<std::uint64_t> culledDraws{ 0 };  // tatsaechlich weggelassen (Stufe 2)
			std::atomic<std::uint64_t> window{ 0 };       // Objekte im laufenden 1-s-Fenster (Hauptbild erkennen)
		};
		std::uint64_t g_cullBlockedFrames = 0;
		std::atomic<bool> g_dumpRequest{ false };
		std::uint32_t     g_dumpCount = 0;

		// Tiefenbild als Graustufen-PGM (hell = nah, logarithmisch bis 100000 Einheiten) neben SPS.log
		void DumpDepth(const Snapshot& a_s)
		{
			const auto dir = SKSE::log::log_directory();
			if (!dir) {
				return;
			}
			const auto  path = *dir / std::format("SPS_OcclusionDepth_{}.pgm", ++g_dumpCount);
			std::ofstream f(path, std::ios::binary);
			f << "P5\n" << OcclusionGpu::kWidth << " " << OcclusionGpu::kHeight << "\n255\n";
			const auto& c = a_s.cam;
			std::uint32_t skyTiles = 0;
			for (std::size_t i = 0; i < a_s.rb.maxDepth.size(); ++i) {
				const float dBuf = c.reversed ? a_s.rb.minDepth[i] : a_s.rb.maxDepth[i];
				float       z = 1e6f;
				if (!(c.reversed ? dBuf <= c.depthMin : dBuf >= c.depthMax)) {
					const float d = (dBuf - c.depthMin) / (c.depthMax - c.depthMin);
					const float den = d - c.depthA;
					z = den != 0.0f ? c.depthB / den : 1e6f;
				} else {
					++skyTiles;
				}
				const float v = z > 1.0f ? 255.0f - std::clamp(std::log(z) / std::log(100000.0f) * 255.0f, 0.0f, 255.0f) : 255.0f;
				f.put(static_cast<char>(static_cast<std::uint8_t>(v)));
			}
			logger::info("[Occlusion] depth image saved: {} (bright = near, farthest value per tile, sky tiles {}, camera ({:.0f}, {:.0f}, {:.0f}), GPU matrix {}, depth A {:.5f} B {:.3f})",
				path.string(), skyTiles, c.pos.x, c.pos.y, c.pos.z, c.gpu ? "yes" : "no", c.depthA, c.depthB);
		}
		// Zum gespeicherten Tiefenbild: alle als verdeckt erkannten Objekte, die mit genau diesem Bild geprueft wurden
		std::atomic<std::uint32_t> g_listSnapId{ 0 };  // 0 = keine Liste offen
		std::uint32_t              g_listNumber = 0;
		std::mutex                 g_listLock;
		std::unordered_map<const void*, std::string> g_list;

		std::string RefInfo(const RE::BSGeometry& a_geom)
		{
			for (const RE::NiAVObject* n = &a_geom; n; n = n->parent) {
				if (const auto ref = n->GetUserData()) {
					const auto base = ref->GetBaseObject();
					return std::format("{:08X};{:08X};{}", ref->GetFormID(), base ? base->GetFormID() : 0u, n->name.c_str() ? n->name.c_str() : "");
				}
			}
			return ";;";
		}

		void WriteList()
		{
			std::unordered_map<const void*, std::string> list;
			{
				std::scoped_lock l(g_listLock);
				list.swap(g_list);
			}
			const auto dir = SKSE::log::log_directory();
			if (!dir) {
				return;
			}
			const auto    path = *dir / std::format("SPS_OcclusionHidden_{}.csv", g_listNumber);
			std::ofstream f(path);
			f << "culled;main;skinned;name;ref;base;node;cx;cy;cz;radius;sx;sy;depth;nearest;u0;u1;v0;v1;farMin;farMax\n";
			for (const auto& [k, line] : list) {
				f << line << "\n";
			}
			logger::info("[Occlusion] list of occluded objects for depth image {} saved: {} ({} objects)", g_listNumber, path.string(), list.size());
		}

		std::mutex                g_sampleLock;
		std::vector<std::string>  g_samples;  // bis 12 weggelassene Objekte je Bericht  // Main-Thread: Frames ohne Weglassen wegen Kamerabewegung
		Clock::time_point g_windowStart = Clock::now();
		AccStats                   g_acc[4];
		std::atomic<std::uint64_t> g_accOther{ 0 }, g_noSnapshot{ 0 }, g_testNs{ 0 };
		// Main-Thread-Zaehler
		std::uint64_t g_prepassCalls = 0, g_camFail = 0, g_captures = 0, g_ringFull = 0, g_noSrv = 0, g_reads = 0, g_latencySum = 0;
		float         g_camFailErrSum = 0.0f;
		const char*   g_camLayout = "unknown";
		// GPU-Matrix vom Beginn des Tiefenvorpasses (Main-Thread)
		CamSnap       g_beginCam{};
		float         g_beginSx = 1.0f, g_beginSy = 1.0f;
		float         g_beginDepthMin = 0.0f, g_beginDepthMax = 1.0f;
		bool          g_beginValid = false;
		std::uint64_t g_beginOk = 0, g_beginFail = 0;

		void Mul(const CamSnap& a_c, const RE::NiPoint3& a_world, float a_out[4]) noexcept
		{
			const float v[4]{ a_c.relative ? a_world.x - a_c.posAdjust.x : a_world.x, a_c.relative ? a_world.y - a_c.posAdjust.y : a_world.y,
				a_c.relative ? a_world.z - a_c.posAdjust.z : a_world.z, 1.0f };
			for (int i = 0; i < 4; ++i) {
				a_out[i] = a_c.colVec ? a_c.m[i][0] * v[0] + a_c.m[i][1] * v[1] + a_c.m[i][2] * v[2] + a_c.m[i][3] * v[3] :
				                        v[0] * a_c.m[0][i] + v[1] * a_c.m[1][i] + v[2] * a_c.m[2][i] + v[3] * a_c.m[3][i];
			}
		}

		// Stichprobe einer Pruefung (Diagnose weggelassener Objekte)
		struct TestInfo
		{
			float x = 0, y = 0, z = 0, wn = 0;
			int   u0 = 0, u1 = 0, v0 = 0, v1 = 0;
			float zFarMin = 0, zFarMax = 0;  // fernste Tiefe der Kacheln als Entfernung, kleinster/groesster Wert
		};

		// 1 = sichtbar, 2 = verdeckt, 3 = zu gross (nicht geprueft)
		int Test(const Snapshot& a_s, const RE::NiBound& a_b, TestInfo* a_info = nullptr) noexcept
		{
			const auto& c = a_s.cam;
			float       x, y, z;
			if (c.gpu) {
				float clip[4];
				Mul(c.gm, a_b.center, clip);
				z = clip[3];
				x = z != 0.0f ? clip[0] / z : 0.0f;
				y = z != 0.0f ? clip[1] / z : 0.0f;
			} else {
				Project(c, a_b.center, x, y, z);
			}
			const float rad = std::max(a_b.radius, 0.0f);
			const float wn = z - rad;
			if (!(wn > 16.0f)) {
				return 1;  // reicht bis an die Kamera oder dahinter
			}
			// Ausdehnung konservativ mit der naechsten Tiefe; x/y aus dem Mittelpunkt (perspektivisch leicht zu klein,
			// deshalb unten eine Kachel Rand)
			const float ex = c.gpu ? rad * c.sx / wn : rad / wn * 2.0f / (c.r - c.l);
			const float ey = c.gpu ? rad * c.sy / wn : rad / wn * 2.0f / (c.t - c.b);
			constexpr float W = static_cast<float>(OcclusionGpu::kWidth), H = static_cast<float>(OcclusionGpu::kHeight);
			// eine Kachel Rand: Bild ist 1-2 Frames alt
			const int u0 = std::max(0, static_cast<int>(std::floor(((x - ex) * 0.5f + 0.5f) * W)) - 1);
			const int u1 = std::min(static_cast<int>(W) - 1, static_cast<int>(std::floor(((x + ex) * 0.5f + 0.5f) * W)) + 1);
			const int v0 = std::max(0, static_cast<int>(std::floor((0.5f - (y + ey) * 0.5f) * H)) - 1);
			const int v1 = std::min(static_cast<int>(H) - 1, static_cast<int>(std::floor((0.5f - (y - ey) * 0.5f) * H)) + 1);
			if (u0 > u1 || v0 > v1) {
				return 1;  // ausserhalb des alten Bildausschnitts - nichts bekannt
			}
			if (static_cast<std::uint32_t>((u1 - u0 + 1) * (v1 - v0 + 1)) > Config::occlMaxTiles.load(std::memory_order_relaxed)) {
				return 3;
			}
			const auto& lo = a_s.rb.minDepth;
			const auto& hi = a_s.rb.maxDepth;
			const float marginK = 1.0f + Config::occlMarginPct.load(std::memory_order_relaxed) * 0.01f;
			const float marginU = Config::occlMarginUnits.load(std::memory_order_relaxed);
			if (a_info) {
				*a_info = { x, y, z, wn, u0, u1, v0, v1, 1e30f, 0.0f };
			}
			for (int v = v0; v <= v1; ++v) {
				const auto row = static_cast<std::size_t>(v) * OcclusionGpu::kWidth;
				for (int u = u0; u <= u1; ++u) {
					// fernste Tiefe der Kachel als Entfernung; verdeckt nur, wenn die naechste Stelle der Kugel klar dahinter
					// liegt (2 % + 16 Einheiten Sicherheitsabstand). Himmel ergibt eine riesige Entfernung -> sichtbar.
					const float dBuf = c.reversed ? lo[row + u] : hi[row + u];
					// Pufferwert ausserhalb des Szenenbereichs = Himmel/geloescht -> sichtbar
					if (c.reversed ? dBuf <= c.depthMin : dBuf >= c.depthMax) {
						return 1;
					}
					const float dFar = (dBuf - c.depthMin) / (c.depthMax - c.depthMin);
					const float den = dFar - c.depthA;
					const float zFar = den != 0.0f ? c.depthB / den : 0.0f;
					if (a_info) {
						a_info->zFarMin = std::min(a_info->zFarMin, zFar);
						a_info->zFarMax = std::max(a_info->zFarMax, zFar);
					}
					if (!(zFar > 0.0f) || !(wn > zFar * marginK + marginU)) {
						return 1;
					}
				}
			}
			return 2;
		}

		AccStats& Slot(const void* a_acc) noexcept
		{
			for (auto& s : g_acc) {
				if (s.acc.load(std::memory_order_relaxed) == a_acc) {
					return s;
				}
			}
			for (auto& s : g_acc) {
				const void* expected = nullptr;
				if (s.acc.compare_exchange_strong(expected, a_acc) || expected == a_acc) {
					return s;
				}
			}
			g_accOther.fetch_add(1, std::memory_order_relaxed);
			return g_acc[3];
		}

		// Messen oder Weglassen eingeschaltet
		bool Active() noexcept
		{
#ifdef SPS_VR
			return false;  // zwei Augen, eigene Tiefen-/Matrix-Wege - nicht erprobt
#endif
			return Config::occlusionProbe.load(std::memory_order_relaxed) || (Config::occlusionCull.load(std::memory_order_relaxed) && Config::masterEnabled.load(std::memory_order_relaxed));
		}

		void Report()
		{
			const auto now = Clock::now();
			if (now - g_reportStart < std::chrono::seconds(60)) {
				return;
			}
			const double secs = std::chrono::duration<double>(now - g_reportStart).count();
			g_reportStart = now;
			// je gerendertem Bild (Tiefenvorpass) - mit Frame Generation gibt es doppelt so viele Presents
			const double frames = std::max<double>(1.0, static_cast<double>(g_prepassCalls ? g_prepassCalls : g_frame));
			const auto   cur = g_current.load();
			std::uint32_t sky = 0;
			if (cur) {
				for (std::size_t i = 0; i < cur->rb.maxDepth.size(); ++i) {
					sky += cur->cam.reversed ? cur->rb.minDepth[i] <= 1e-6f : cur->rb.maxDepth[i] >= 0.99999f;
				}
			}
			logger::info("[Occlusion] {:.0f} s, {:.0f} frames | prepass {} | camera matrix {} usable {} frames, not {} (avg w error {:.0f}) | GPU matrix at prepass start {} ok / {} not, depth range {:.4f}-{:.4f} | own projection: right {:+.0f} up {:+.0f} (compared {} frames), depth {} A {:.5f} B {:.3f}{}, max NDC diff {:.3f} | depth source {} ({}x{}) | captures {}, ring full {}, no depth {} | reads {}, latency {:.1f} frames | sky tiles {:.0f} % | test time {:.2f} ms/frame (all threads) | no snapshot yet {}",
				secs, frames, g_prepassCalls, g_camLayout, g_learnFrames, g_camFail, g_camFail ? g_camFailErrSum / g_camFail : 0.0f, g_beginOk, g_beginFail, g_beginDepthMin, g_beginDepthMax, g_rightSign, g_upSign, g_gpuSignFrames,
				g_learned ? "learned" : "formula", g_learnA, g_learnB, g_learnRev ? " reversed" : "", g_learnMaxErr, g_lastSource == 0 ? "post-prepass copy" : "main",
				g_lastSrcW, g_lastSrcH, g_captures, g_ringFull, g_noSrv, g_reads, g_reads ? static_cast<double>(g_latencySum) / g_reads : 0.0,
				cur ? 100.0 * sky / cur->rb.maxDepth.size() : 0.0, g_testNs.exchange(0) / 1e6 / frames, g_noSnapshot.exchange(0));
			for (auto& s : g_acc) {
				const auto geoms = s.geoms.exchange(0);
				const auto acc = s.acc.exchange(nullptr);
				const auto draws = s.draws.exchange(0), tested = s.tested.exchange(0), occGeoms = s.occGeoms.exchange(0), occDraws = s.occDraws.exchange(0);
				const auto culled = s.culledDraws.exchange(0);
				const auto tooBig = s.tooBig.exchange(0), occSkinned = s.occSkinned.exchange(0), occSmall = s.occSmall.exchange(0), occMid = s.occMid.exchange(0), occLarge = s.occLarge.exchange(0);
				if (!acc || !geoms) {
					continue;
				}
				logger::info("[Occlusion]   accumulator {} | per frame: objects {:.0f}, draws {:.0f}, tested {:.0f} | occluded: objects {:.0f} ({:.0f} % of tested), draws {:.0f} ({:.0f} % of all) | occluded draws by radius <50 {:.0f}, <200 {:.0f}, larger {:.0f}, skinned {:.0f} | too big {:.0f} | culled draws {:.0f}{}",
					acc, geoms / frames, draws / frames, tested / frames, occGeoms / frames, tested ? 100.0 * occGeoms / tested : 0.0, occDraws / frames,
					draws ? 100.0 * occDraws / draws : 0.0, occSmall / frames, occMid / frames, occLarge / frames, occSkinned / frames, tooBig / frames, culled / frames, acc == g_mainAcc.load() ? " (main view)" : "");
			}
			g_accOther = 0;
			g_prepassCalls = g_camFail = g_captures = g_ringFull = g_noSrv = g_reads = g_latencySum = 0;
			logger::info("[Occlusion]   hiding occluded objects: {} | frames without hiding because the camera moved: {} | settings: pause from {:.1f} deg / {:.0f} units, margin {:.1f} % + {:.0f}, hidden in {} images in a row, max {} tiles, characters {}", Config::occlusionCull.load() && Config::masterEnabled.load() ? "ON" : "off", g_cullBlockedFrames, Config::occlTurnDeg.load(), Config::occlMoveUnits.load(), Config::occlMarginPct.load(), Config::occlMarginUnits.load(), Config::occlStreak.load(), Config::occlMaxTiles.load(), Config::occlSkipSkinned.load() ? "never" : "allowed");
			g_cullBlockedFrames = 0;
			{
				std::scoped_lock l(g_sampleLock);
				for (const auto& smp : g_samples) {
					logger::info("[Occlusion]     hidden: {}", smp);
				}
				g_samples.clear();
			}
			ClearStreaks();  // Verlauf nicht unbegrenzt wachsen lassen (wiederverwendete Objekt-Adressen)
			g_camFailErrSum = 0.0f;
			g_learnFrames = 0;
			g_gpuSignFrames = 0;
			g_beginOk = g_beginFail = 0;
			g_learnMaxErr = 0.0f;
			g_frame = 0;
		}
	}

	void OnDepthPrepassBegin() noexcept
	{
		g_beginValid = false;
		if (!Active()) {
			return;
		}
		const auto state = RE::BSGraphics::RendererShadowState::GetSingleton();
		const auto cam = RE::Main::WorldRootCamera();
		if (!state || !cam) {
			return;
		}
		auto&      rd = state->GetRuntimeData();
		const auto view = rd.cameraData.getEye();
		CamSnap    g{};
		std::memcpy(g.m, &view.viewProjMatrixUnjittered, sizeof(g.m));
		g.posAdjust = rd.posAdjust.getEye();
		// gehoert die Matrix zur Hauptkamera? Ein Punkt 1000 Einheiten vor der Kamera muss w ~ 1000 ergeben
		const auto&        pos = cam->world.translate;
		const auto&        rot = cam->world.rotate;
		const RE::NiPoint3 p{ pos.x + rot.entry[0][0] * 1000.0f, pos.y + rot.entry[1][0] * 1000.0f, pos.z + rot.entry[2][0] * 1000.0f };
		float              best = 1e9f;
		for (int k = 0; k < 4; ++k) {
			CamSnap t = g;
			t.colVec = (k & 1) == 0;
			t.relative = (k & 2) == 0;
			float c[4];
			Mul(t, p, c);
			if (const float err = std::abs(c[3] - 1000.0f); err < best) {
				best = err;
				g.colVec = t.colVec;
				g.relative = t.relative;
			}
		}
		if (best > 20.0f) {
			++g_beginFail;
			return;
		}
		g_beginCam = g;
		g_beginSx = std::abs(view.projMatrixUnjittered.m[0][0]);
		g_beginSy = std::abs(view.projMatrixUnjittered.m[1][1]);
		g_beginDepthMin = view.viewDepthRange.x;
		g_beginDepthMax = view.viewDepthRange.y;
		if (!(g_beginDepthMax > g_beginDepthMin) || g_beginDepthMin < 0.0f || g_beginDepthMax > 1.0f) {
			++g_beginFail;  // unbrauchbarer Bereich - lieber nicht weglassen
			return;
		}
		g_beginValid = true;
		++g_beginOk;
	}

	void OnDepthPrepassEnd() noexcept
	{
		if (!Active()) {
			return;
		}
		++g_prepassCalls;
		const auto state = RE::BSGraphics::RendererShadowState::GetSingleton();
		const auto cam = RE::Main::WorldRootCamera();
		if (!state || !cam) {
			return;
		}
		// Eigene Projektion: Gamebryo-Kamera blickt entlang Spalte 0, Spalte 1 = oben, Spalte 2 = rechts
		const auto& camPos = cam->world.translate;
		const auto& rot = cam->world.rotate;
		const auto& fr = cam->GetRuntimeData2().viewFrustum;
		if (fr.bOrtho || !(fr.fRight > fr.fLeft) || !(fr.fTop > fr.fBottom) || !(fr.fFar > fr.fNear) || !(fr.fNear > 0.0f)) {
			++g_camFail;
			return;
		}
		auto& own = g_snaps[g_nextTag % kSnaps];
		own = {};
		own.pos = camPos;
		own.dir = { rot.entry[0][0], rot.entry[1][0], rot.entry[2][0] };
		own.up = { rot.entry[0][1] * g_upSign, rot.entry[1][1] * g_upSign, rot.entry[2][1] * g_upSign };
		own.right = { rot.entry[0][2] * g_rightSign, rot.entry[1][2] * g_rightSign, rot.entry[2][2] * g_rightSign };
		own.l = fr.fLeft;
		own.r = fr.fRight;
		own.t = fr.fTop;
		own.b = fr.fBottom;
		if (g_learned) {
			own.depthA = g_learnA;
			own.depthB = g_learnB;
			own.reversed = g_learnRev;
		} else {
			own.depthA = fr.fFar / (fr.fFar - fr.fNear);
			own.depthB = -fr.fFar * fr.fNear / (fr.fFar - fr.fNear);
		}
		own.frame = g_frame;
		own.valid = true;
		if (g_beginValid) {
			// das Tiefenbild entsteht mit dieser Matrix - Projektion und Tiefenformel direkt daraus
			own.gpu = true;
			own.gm = g_beginCam;
			own.sx = g_beginSx;
			own.sy = g_beginSy;
			own.depthMin = g_beginDepthMin;
			own.depthMax = g_beginDepthMax;
			const RE::NiPoint3 a{ camPos.x + own.dir.x * 100.0f, camPos.y + own.dir.y * 100.0f, camPos.z + own.dir.z * 100.0f };
			const RE::NiPoint3 b{ camPos.x + own.dir.x * 10000.0f, camPos.y + own.dir.y * 10000.0f, camPos.z + own.dir.z * 10000.0f };
			float c1[4], c2[4];
			Mul(own.gm, a, c1);
			Mul(own.gm, b, c2);
			const float d1 = c1[2] / c1[3], d2 = c2[2] / c2[3];
			own.reversed = d1 > d2;
			own.depthB = (d1 - d2) / (1.0f / c1[3] - 1.0f / c2[3]);
			own.depthA = d1 - own.depthB / c1[3];
		}
		g_beginValid = false;
		g_snapPending = true;

		// Weglassen nur, wenn sich die Kamera seit dem geltenden Tiefenbild kaum bewegt hat (< 2 Grad, < 64 Einheiten)
		{
			bool       allowed = false;
			const auto cur = g_current.load();
			if (cur) {
				const auto& oc = cur->cam;
				const float dx = own.pos.x - oc.pos.x, dy = own.pos.y - oc.pos.y, dz = own.pos.z - oc.pos.z;
				const float move = Config::occlMoveUnits.load(std::memory_order_relaxed);
				allowed = Dot(own.dir, oc.dir) > std::cos(Config::occlTurnDeg.load(std::memory_order_relaxed) * 0.0174533f) && dx * dx + dy * dy + dz * dz < move * move;
			}
			if (!allowed) {
				++g_cullBlockedFrames;
			}
			g_cullAllowed.store(allowed, std::memory_order_relaxed);
		}

		auto&              rd = state->GetRuntimeData();
		const auto         view = rd.cameraData.getEye();
		const RE::NiPoint3 posAdj = rd.posAdjust.getEye();
		const auto         at = [&](float a_d) { return RE::NiPoint3{ camPos.x + own.dir.x * a_d, camPos.y + own.dir.y * a_d, camPos.z + own.dir.z * a_d }; };
		// Konvention einer Matrix bestimmen: ein Punkt 1000 Einheiten vor der Kamera muss w ~ 1000 ergeben
		const auto understand = [&](CamSnap& a_s) {
			float best = 1e9f;
			bool  col = true, rel = true;
			for (int k = 0; k < 4; ++k) {
				CamSnap t = a_s;
				t.colVec = (k & 1) == 0;
				t.relative = (k & 2) == 0;
				float c[4];
				Mul(t, at(1000.0f), c);
				const float err = std::abs(c[3] - 1000.0f);
				if (err < best) {
					best = err;
					col = t.colVec;
					rel = t.relative;
				}
			}
			a_s.colVec = col;
			a_s.relative = rel;
			return best;
		};

		// 1) Tiefenformel aus worldToCam der Spielkamera (gehoert sicher zur Hauptkamera)
		CamSnap s{};
		std::memcpy(s.m, cam->GetRuntimeData().worldToCam, sizeof(s.m));
		s.posAdjust = posAdj;
		if (const float best = understand(s); best <= 50.0f) {
			g_camLayout = s.colVec ? (s.relative ? "col/rel" : "col/abs") : (s.relative ? "row/rel" : "row/abs");
			float c1[4], c2[4];
			Mul(s, at(100.0f), c1);
			Mul(s, at(10000.0f), c2);
			const float d1 = c1[2] / c1[3], d2 = c2[2] / c2[3];
			g_learnRev = d1 > d2;
			g_learnB = (d1 - d2) / (1.0f / c1[3] - 1.0f / c2[3]);
			g_learnA = d1 - g_learnB / c1[3];
			g_learned = true;
			++g_learnFrames;
		} else {
			++g_camFail;
			g_camFailErrSum += std::min(best, 1e6f);
		}

		// 2) Diagnose: eigene Projektion gegen die GPU-Matrix vom Beginn des Tiefenvorpasses (Achsen werden nicht mehr
		// daraus gelernt - ein falscher Treffer am Ende des Vorpasses drehte im Test beide Achsen um)
		if (own.gpu) {
			const RE::NiPoint3 rawUp{ rot.entry[0][1], rot.entry[1][1], rot.entry[2][1] };
			const RE::NiPoint3 rawRight{ rot.entry[0][2], rot.entry[1][2], rot.entry[2][2] };
			const RE::NiPoint3 p{ camPos.x + own.dir.x * 1000.0f + rawRight.x * 200.0f + rawUp.x * 100.0f, camPos.y + own.dir.y * 1000.0f + rawRight.y * 200.0f + rawUp.y * 100.0f,
				camPos.z + own.dir.z * 1000.0f + rawRight.z * 200.0f + rawUp.z * 100.0f };
			float rc[4];
			Mul(own.gm, p, rc);
			float ox, oy, oz;
			Project(own, p, ox, oy, oz);
			g_learnMaxErr = std::max({ g_learnMaxErr, std::abs(rc[0] / rc[3] - ox), std::abs(rc[1] / rc[3] - oy) });
			++g_gpuSignFrames;
		}
	}
	void OnPresent() noexcept
	{
		++g_frame;
		// Tracy-Marke bei jeder Aenderung von Schalter/Stellschrauben (Abschnitte einer Aufnahme zuordnen)
		{
			static std::string       last, pending;
			static Clock::time_point pendingSince{};
			auto cur = std::format("Occlusion: hiding {} | measure {} | pause {:.1f} deg / {:.0f} | margin {:.1f} % + {:.0f} | streak {} | max tiles {} | characters {}",
				Config::occlusionCull.load() && Config::masterEnabled.load() ? "ON" : "off", Config::occlusionProbe.load() ? "on" : "off", Config::occlTurnDeg.load(),
				Config::occlMoveUnits.load(), Config::occlMarginPct.load(), Config::occlMarginUnits.load(), Config::occlStreak.load(), Config::occlMaxTiles.load(),
				Config::occlSkipSkinned.load() ? "never" : "allowed");
			// erst melden, wenn der Wert 0,5 s steht (Regler ziehen erzeugt sonst eine Flut)
			if (cur != pending) {
				pending = std::move(cur);
				pendingSince = Clock::now();
			} else if (pending != last && Clock::now() - pendingSince >= std::chrono::milliseconds(500)) {
				TracyMessageC(pending.data(), pending.size(), 0x40FF40);
				logger::info("[Occlusion] {}", pending);
				last = pending;
			}
		}
		if (!Active()) {
			g_current.store(nullptr);
			return;
		}
		const auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer) {
			return;
		}
		const auto ctx = renderer->GetRuntimeData().context;
		if (!OcclusionGpu::Ready()) {
			if (g_initTried) {
				return;
			}
			g_initTried = true;
			char err[256]{};
			if (!OcclusionGpu::Init(renderer->GetRuntimeData().forwarder, err, sizeof(err))) {
				logger::warn("[Occlusion] init failed: {}", err);
				return;
			}
			logger::info("[Occlusion] depth image ready ({}x{} tiles)", OcclusionGpu::kWidth, OcclusionGpu::kHeight);
		}
		// fertige Ergebnisse abholen (das neueste gilt) und fuer alle Threads veroeffentlichen
		OcclusionGpu::Readback rb;
		std::shared_ptr<Snapshot> newest;
		while (OcclusionGpu::Poll(ctx, rb)) {
			const auto& snap = g_snaps[rb.tag % kSnaps];
			if (!snap.valid) {
				continue;
			}
			newest = std::make_shared<Snapshot>();
			newest->rb = std::move(rb);
			newest->cam = snap;
			g_lastSrcW = newest->rb.srcW;
			g_lastSrcH = newest->rb.srcH;
			++g_reads;
			g_latencySum += g_frame - snap.frame;
		}
		if (newest) {
			newest->id = g_nextSnapshotId++;
			g_current.store(std::move(newest));
		}
		if (const auto listId = g_listSnapId.load(); listId != 0) {
			const auto cur = g_current.load();
			if (!cur || cur->id != listId) {
				g_listSnapId.store(0);
				WriteList();
			}
		}
		if (g_dumpRequest.exchange(false)) {
			if (const auto cur = g_current.load()) {
				DumpDepth(*cur);
				g_listNumber = g_dumpCount;
				{
					std::scoped_lock l(g_listLock);
					g_list.clear();
				}
				g_listSnapId.store(cur->id);
			}
		}
		// Hauptbild = Accumulator mit den meisten Objekten in der letzten Sekunde
		if (const auto now = Clock::now(); now - g_windowStart >= std::chrono::seconds(1)) {
			g_windowStart = now;
			const void*   best = nullptr;
			std::uint64_t most = 0;
			for (auto& s : g_acc) {
				const auto n = s.window.exchange(0);
				if (n > most) {
					most = n;
					best = s.acc.load();
				}
			}
			g_mainAcc.store(best);
		}
		// Tiefe dieses Frames einreichen
		if (g_snapPending) {
			g_snapPending = false;
			const auto source = Config::occlusionDepthSource.load(std::memory_order_relaxed);
			const auto idx = source == 0 ? RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY : RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN;
			const auto srv = renderer->GetDepthStencilData().depthStencils[idx].depthSRV;
			g_lastSource = source;
			if (!srv) {
				++g_noSrv;
			} else if (OcclusionGpu::Capture(ctx, srv, g_nextTag)) {
				++g_captures;
				++g_nextTag;
			} else {
				++g_ringFull;
			}
		}
		Report();
	}

	bool CountMain(const RE::BSGeometry& a_geom, const void* a_accumulator, std::uint32_t a_draws) noexcept
	{
#ifdef SPS_VR
		return false;
#endif
		const bool cullOn = Config::occlusionCull.load(std::memory_order_relaxed) && Config::masterEnabled.load(std::memory_order_relaxed);
		if (!Config::occlusionProbe.load(std::memory_order_relaxed) && !cullOn) {
			return false;
		}
		auto& s = Slot(a_accumulator);
		s.geoms.fetch_add(1, std::memory_order_relaxed);
		s.draws.fetch_add(a_draws, std::memory_order_relaxed);
		s.window.fetch_add(1, std::memory_order_relaxed);
		const auto snap = g_current.load();
		if (!snap) {
			g_noSnapshot.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		const auto t0 = Clock::now();
		const int  r = Test(*snap, a_geom.worldBound);
		g_testNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count()), std::memory_order_relaxed);
		if (r == 3) {
			s.tooBig.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		s.tested.fetch_add(1, std::memory_order_relaxed);
		const bool skinned = const_cast<RE::BSGeometry&>(a_geom).GetGeometryRuntimeData().skinInstance != nullptr;
		const bool isMain = a_accumulator && a_accumulator == g_mainAcc.load(std::memory_order_relaxed);
		// Verlauf nur fuer feste Objekte im Hauptbild (Figuren: Huelle hinkt der Animation hinterher)
		const bool twice = isMain && !(skinned && Config::occlSkipSkinned.load(std::memory_order_relaxed)) && UpdateStreak(&a_geom, snap->id, r == 2);
		if (r != 2) {
			return false;
		}
		s.occGeoms.fetch_add(1, std::memory_order_relaxed);
		s.occDraws.fetch_add(a_draws, std::memory_order_relaxed);
		const float rad = a_geom.worldBound.radius;
		(rad < 50.0f ? s.occSmall : rad < 200.0f ? s.occMid : s.occLarge).fetch_add(a_draws, std::memory_order_relaxed);
		if (skinned) {
			s.occSkinned.fetch_add(a_draws, std::memory_order_relaxed);
		}
		const bool willCull = cullOn && twice && snap->cam.gpu && g_cullAllowed.load(std::memory_order_relaxed);
		if (snap->id == g_listSnapId.load(std::memory_order_relaxed)) {
			TestInfo    info;
			Test(*snap, a_geom.worldBound, &info);
			const auto& b = a_geom.worldBound;
			auto        line = std::format("{};{};{};{};{};{:.0f};{:.0f};{:.0f};{:.0f};{:.3f};{:.3f};{:.0f};{:.0f};{};{};{};{};{:.0f};{:.0f}",
					   willCull ? 1 : 0, isMain ? 1 : 0, skinned ? 1 : 0, a_geom.name.c_str() ? a_geom.name.c_str() : "", RefInfo(a_geom), b.center.x, b.center.y, b.center.z, b.radius,
					   info.x, info.y, info.z, info.wn, info.u0, info.u1, info.v0, info.v1, info.zFarMin, info.zFarMax);
			std::scoped_lock l(g_listLock);
			if (g_list.size() < 20000) {
				g_list.try_emplace(&a_geom, std::move(line));
			}
		}
		if (willCull) {
			s.culledDraws.fetch_add(a_draws, std::memory_order_relaxed);
			{
				std::scoped_lock l(g_sampleLock);
				if (g_samples.size() < 12) {
					TestInfo info;
					Test(*snap, a_geom.worldBound, &info);
					const auto& b = a_geom.worldBound;
					g_samples.push_back(std::format("'{}' center ({:.0f}, {:.0f}, {:.0f}) radius {:.0f} | screen ({:.2f}, {:.2f}) depth {:.0f}, nearest {:.0f} | tiles x {}-{} y {}-{} | farthest depth in tiles {:.0f}-{:.0f} | image {} frames old",
						a_geom.name.c_str() ? a_geom.name.c_str() : "", b.center.x, b.center.y, b.center.z, b.radius, info.x, info.y, info.z, info.wn,
						info.u0, info.u1, info.v0, info.v1, info.zFarMin, info.zFarMax, static_cast<long long>(g_frame) - static_cast<long long>(snap->cam.frame)));
				}
			}
			return true;
		}
		return false;
	}

	void RequestDepthDump() noexcept { g_dumpRequest.store(true); }

	void Reset() noexcept
	{
		g_current.store(nullptr);
		g_snapPending = false;
		g_cullAllowed.store(false);
		OcclusionGpu::Reset();
		ClearStreaks();
	}
}
