#include "Occlusion.h"
#include "ShadowCulling.h"

#include "Config.h"
#include "OcclusionGpu.h"

#include <condition_variable>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <mutex>
#include <thread>
#include <unordered_map>

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
			RE::NiMatrix3 rot{};  // Lage der Weltkamera (Spalte 0 = Blick) - fuers Umrechnen in die Kamera eines spaeteren Frames
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
			// Bereich der Szene im Tiefenpuffer: mit Upscaler (DLSS/FSR ueber Community Shaders) oder dynamischer
			// Aufloesung nur ein Teil (links oben). Ohne das passten Tiefenbild und Objekte nicht zusammen ->
			// sichtbare Objekte wurden weggelassen (grau-blaue Flaechen).
			float         vp[4]{};                   // Viewport des Tiefenvorpasses (x, y, Breite, Hoehe)
			// Spielerfigur (oder Reittier) beim Tiefenbild: bewegt sich mit der Kamera und darf beim Umrechnen nicht als feste
			// Verdeckung an der alten Stelle liegen bleiben
			std::vector<RE::NiBound> movers;  // Spieler, Reittier, Figuren in der Naehe: Mitte + Radius
			float         resW = 1.0f, resH = 1.0f;  // dynamische Aufloesung der Engine
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
		// Erhoeht bei jedem neuen Tiefenbild. Die Pruef-Threads holen g_current nur bei Aenderung (atomarer shared_ptr
		// = interne Sperre + Referenzzaehler auf gemeinsamer Cache-Zeile; bis 1.0.57 je Objekt, ~2000x pro Frame)
		std::atomic<std::uint64_t>                   g_currentGen{ 1 };

		void Publish(std::shared_ptr<const Snapshot> a_snap) noexcept
		{
			g_current.store(std::move(a_snap));
			g_currentGen.fetch_add(1, std::memory_order_release);
		}

		// Weltkamera beim Sammeln (dieser Frame) nah an der Kamera des Tiefenbilds? Waehrend des Sammelns aendert sie sich nicht.
		bool CameraStillNear(const OwnCam& a_snap) noexcept
		{
			const auto cam = RE::Main::WorldRootCamera();
			if (!cam) {
				return false;
			}
			const auto&        w = cam->world;
			const RE::NiPoint3 dir{ w.rotate.entry[0][0], w.rotate.entry[1][0], w.rotate.entry[2][0] };
			const float        dx = w.translate.x - a_snap.pos.x, dy = w.translate.y - a_snap.pos.y, dz = w.translate.z - a_snap.pos.z;
			const float        move = Config::occlMoveUnits.load(std::memory_order_relaxed);
			return Dot(dir, a_snap.dir) > std::cos(Config::occlTurnDeg.load(std::memory_order_relaxed) * 0.0174533f) && dx * dx + dy * dy + dz * dz < move * move;
		}

		// Aktuelles Tiefenbild fuer diesen Thread (haelt das alte bis zum naechsten Wechsel am Leben)
		const Snapshot* CurrentForThread() noexcept
		{
			thread_local std::shared_ptr<const Snapshot> snap;
			thread_local std::uint64_t                   gen = 0;
			const auto                                   now = g_currentGen.load(std::memory_order_acquire);
			if (now != gen) {
				snap = g_current.load();
				gen = now;
			}
			return snap.get();
		}
		std::uint32_t                                g_nextSnapshotId = 1;  // Main-Thread

		// Stufe 2 (Weglassen): nur im Accumulator des Hauptbilds (der mit den meisten Objekten je Sekunde), nur feste
		// Objekte, erst nach zwei Tiefenbildern hintereinander verdeckt, nicht bei schneller Kamerabewegung.
		std::atomic<const void*> g_mainAcc{ nullptr };
		std::atomic<bool>        g_cullAllowed{ false };  // Kamera seit dem Tiefenbild kaum bewegt (Main-Thread setzt)
		struct Streak
		{
			std::uint32_t lastId = 0;  // Tiefenbild der letzten Pruefung
			std::uint8_t  count = 0;   // so viele Tiefenbilder hintereinander verdeckt
			float         x = 0, y = 0, z = 0;  // Mittelpunkt bei der letzten Pruefung
		};
		struct Shard
		{
			std::mutex                                     lock;
			std::unordered_map<const void*, Streak>        map;
		};
		constexpr std::size_t kShards = 32;
		Shard                 g_shards[kShards];

		// true = in den letzten Tiefenbildern verdeckt und seitdem nicht bewegt. Bewegte Objekte (Fische, Tueren,
		// Physik-Objekte) stehen im alten Tiefenbild woanders -> nie weglassen (Beta 1.0.52: Fische wurden grau)
		bool UpdateStreak(const void* a_geom, std::uint32_t a_snapId, bool a_occluded, const RE::NiPoint3& a_center) noexcept
		{
			auto&            sh = g_shards[(reinterpret_cast<std::uintptr_t>(a_geom) >> 4) % kShards];
			std::scoped_lock l(sh.lock);
			// Sichtbare Objekte (die meisten Pruefungen) brauchen keinen Eintrag: ohne Verlauf beginnt die naechste Verdeckung
			// ohnehin bei 1 - so bleibt die Tabelle klein und es wird seltener Speicher angefordert (Review 1.0.69, L4)
			if (!a_occluded) {
				if (const auto it = sh.map.find(a_geom); it != sh.map.end()) {
					sh.map.erase(it);
				}
				return false;
			}
			auto&            e = sh.map[a_geom];
			const auto       need = Config::occlStreak.load(std::memory_order_relaxed);
			if (e.lastId == a_snapId) {
				// mehrere Passes desselben Objekts im selben Frame - oder dasselbe Bild noch einmal (Rueckleseweg hing): ein
				// inzwischen bewegtes Objekt verliert seinen Zaehler (es koennte hinter seiner eigenen alten Lage stecken)
				const float mx = a_center.x - e.x, my = a_center.y - e.y, mz = a_center.z - e.z;
				if (mx * mx + my * my + mz * mz > 1.0f) {
					e.count = 0;
					e.x = a_center.x;
					e.y = a_center.y;
					e.z = a_center.z;
					return false;
				}
				return a_occluded && e.count >= need;
			}
			const float dx = a_center.x - e.x, dy = a_center.y - e.y, dz = a_center.z - e.z;
			const bool  moved = e.lastId != 0 && dx * dx + dy * dy + dz * dz > 1.0f;
			e.x = a_center.x;
			e.y = a_center.y;
			e.z = a_center.z;
			if (!a_occluded || moved) {
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

		// Draw-Filter: im Hauptbild verdeckte Objekte dieses Bilds. Offene Adressierung ohne Sperre; Eintraege mit altem
		// Bild-Zaehler gelten als frei. Geht ein Eintrag im Wettlauf verloren, wird das Objekt einfach gezeichnet.
		struct HiddenSlot
		{
			std::atomic<const void*>   key{ nullptr };
			std::atomic<std::uint32_t> stamp{ 0 };
		};
		constexpr std::size_t                     kHiddenSlots = 1u << 14;
		constexpr std::size_t                     kHiddenProbe = 32;
		std::array<HiddenSlot, kHiddenSlots>      g_hidden{};
		std::atomic<std::uint32_t>                g_hiddenStamp{ 1 };
		std::atomic<std::uint64_t>                g_hiddenMarked{ 0 }, g_hiddenFull{ 0 }, g_filterSkipped{ 0 };
		std::atomic<std::uint32_t>                g_markedStamp{ 0 };  // Bild, in dem zuletzt etwas markiert wurde

		std::size_t HiddenHash(const void* a_p) noexcept
		{
			auto h = reinterpret_cast<std::uintptr_t>(a_p) >> 4;
			h ^= h >> 17;
			h *= 0x9E3779B97F4A7C15ull;
			return static_cast<std::size_t>(h >> 40);
		}
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
		std::uint64_t g_poseMismatch = 0;  // Main-Thread: Tiefenbilder verworfen, weil Kamera-Lage und GPU-Matrix nicht zusammenpassten
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

		// Sichttiefe eines Pufferwerts (Kachel); <= 0 = Himmel/ungueltig
		float BufToZ(const OwnCam& a_c, float a_buf) noexcept
		{
			if (a_c.reversed ? a_buf <= a_c.depthMin : a_buf >= a_c.depthMax) {
				return 0.0f;
			}
			const float d = (a_buf - a_c.depthMin) / (a_c.depthMax - a_c.depthMin);
			const float den = d - a_c.depthA;
			return den != 0.0f ? a_c.depthB / den : 0.0f;
		}

		// 1 = sichtbar, 2 = verdeckt, 3 = zu gross (nicht geprueft)
		// a_camNow: Kamera des Frames, in dem weggelassen wuerde. Das Tiefenbild ist 1-3 Bilder alt; hat sich die Kamera
		// seitdem verschoben, wandert eine nahe Verdeckung auf dem Bild deutlich weiter als das Objekt dahinter (Parallaxe).
		// Ohne Ausgleich tauchte beim Laufen um eine Ecke oder einen Pfeiler kurz der Himmel auf (1.0.66).
		int Test(const Snapshot& a_s, const RE::NiBound& a_b, TestInfo* a_info = nullptr, const RE::NiPoint3* a_camNow = nullptr) noexcept
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
			// Bildausdehnung ueber die 8 Ecken des Wuerfels um die Kugel (enthaelt die Kugel, Projektion damit sicher gross
			// genug). Bis 1.0.55 Mittelpunkt +- Radius/Tiefe: am Bildrand perspektivisch deutlich zu klein (bei 45 Grad
			// ~10 Kacheln) -> halb verdeckte grosse Objekte am Rand konnten verschwinden.
			float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
			for (int i = 0; i < 8; ++i) {
				const RE::NiPoint3 p{ a_b.center.x + ((i & 1) ? rad : -rad), a_b.center.y + ((i & 2) ? rad : -rad), a_b.center.z + ((i & 4) ? rad : -rad) };
				float px, py, pw;
				if (c.gpu) {
					float clip[4];
					Mul(c.gm, p, clip);
					pw = clip[3];
					px = pw != 0.0f ? clip[0] / pw : 0.0f;
					py = pw != 0.0f ? clip[1] / pw : 0.0f;
				} else {
					Project(c, p, px, py, pw);
				}
				if (!(pw > 1.0f)) {
					return 1;  // Ecke an oder hinter der Kamera - Ausdehnung unbekannt
				}
				minX = std::min(minX, px);
				maxX = std::max(maxX, px);
				minY = std::min(minY, py);
				maxY = std::max(maxY, py);
			}
			constexpr int W = static_cast<int>(OcclusionGpu::kWidth), H = static_cast<int>(OcclusionGpu::kHeight);
			// eine Kachel Rand: Bild ist 1-2 Frames alt
			int u0 = static_cast<int>(std::floor((minX * 0.5f + 0.5f) * W)) - 1;
			int u1 = static_cast<int>(std::floor((maxX * 0.5f + 0.5f) * W)) + 1;
			int v0 = static_cast<int>(std::floor((0.5f - maxY * 0.5f) * H)) - 1;
			int v1 = static_cast<int>(std::floor((0.5f - minY * 0.5f) * H)) + 1;
			// Reicht das Objekt ueber den Rand des alten Bilds hinaus, ist der Teil draussen unbekannt (beim Drehen kommt er
			// ins Bild) - bis 1.0.66 wurde nur der Teil im Bild geprueft und das Objekt fehlte kurz am Bildrand
			if (u0 < 0 || v0 < 0 || u1 > W - 1 || v1 > H - 1) {
				return 1;
			}
			if (static_cast<std::uint32_t>((u1 - u0 + 1) * (v1 - v0 + 1)) > Config::occlMaxTiles.load(std::memory_order_relaxed)) {
				return 3;
			}
			const auto& lo = a_s.rb.minDepth;
			const auto& hi = a_s.rb.maxDepth;
			// Parallaxe seit dem Tiefenbild: eine Verdeckung in Tiefe zNear verschiebt sich um ~ Weg / zNear (Bogenmass)
			// gegenueber dem Objekt -> Pruefbereich um so viele Kacheln vergroessern (naechste Tiefe im Bereich = groesste
			// Verschiebung). Das Objekt muss dann auch dort verdeckt sein, wohin die Kante gewandert sein kann.
			if (a_camNow) {
				const float mx = a_camNow->x - c.pos.x, my = a_camNow->y - c.pos.y, mz = a_camNow->z - c.pos.z;
				const float move = std::sqrt(mx * mx + my * my + mz * mz);
				if (move > 0.5f) {
					const float sx = c.gpu ? c.sx : 2.0f / (c.r - c.l);
					const float sy = c.gpu ? c.sy : 2.0f / (c.t - c.b);
					// am Bildrand verschiebt sich ein Punkt staerker als in der Mitte (1 + x^2 bezogen auf die Projektion)
					const float edgeX = 1.0f + std::max(minX * minX, maxX * maxX) / (sx * sx);
					const float edgeY = 1.0f + std::max(minY * minY, maxY * maxY) / (sy * sy);
					// zweimal: die naechste Verdeckung kann auch knapp neben dem Objekt liegen (Pfeilerkante) - im
					// zweiten Durchgang wird im schon vergroesserten Bereich gesucht. Kostengrenze: in Bewegung nur Bereiche
					// bis kParallaxTiles pruefen, sonst gilt das Objekt als sichtbar (vor einem nahen Pfosten wurden sonst
					// je Objekt Tausende Kacheln durchsucht -> Einbruch beim Laufen, 1.0.66-Test)
					constexpr std::uint32_t kParallaxTiles = 512;
					const auto area = [&](int a_du, int a_dv) {
						return static_cast<std::uint32_t>((u1 - u0 + 1 + 2 * a_du) * (v1 - v0 + 1 + 2 * a_dv));
					};
					if (area(0, 0) > kParallaxTiles) {
						return 1;
					}
					int du = 0, dv = 0;
					for (int pass = 0; pass < 2; ++pass) {
						if (u0 - du < 0 || v0 - dv < 0 || u1 + du > W - 1 || v1 + dv > H - 1) {
							return 1;  // die Kante kann aus dem bekannten Bild gewandert sein
						}
						if (area(du, dv) > kParallaxTiles) {
							return 1;  // zu viel Bewegung fuer eine sichere Aussage
						}
						float zNear = 1e30f;
						for (int v = std::max(0, v0 - dv); v <= std::min(H - 1, v1 + dv); ++v) {
							const auto row = static_cast<std::size_t>(v) * OcclusionGpu::kWidth;
							for (int u = std::max(0, u0 - du); u <= std::min(W - 1, u1 + du); ++u) {
								const float zTile = BufToZ(c, c.reversed ? hi[row + u] : lo[row + u]);
								if (zTile > 0.0f) {
									zNear = std::min(zNear, zTile);
								}
							}
						}
						if (!(zNear < 1e29f)) {
							break;
						}
						const float ang = move / std::max(zNear, 1.0f);
						du = std::max(du, static_cast<int>(std::ceil(ang * sx * edgeX * 0.5f * W)) + 2);
						dv = std::max(dv, static_cast<int>(std::ceil(ang * sy * edgeY * 0.5f * H)) + 2);
					}
					if (du || dv) {
						u0 -= du;
						u1 += du;
						v0 -= dv;
						v1 += dv;
						if (u0 < 0 || v0 < 0 || u1 > W - 1 || v1 > H - 1) {
							return 1;  // die Kante kann aus dem bekannten Bild gewandert sein
						}
						if (static_cast<std::uint32_t>((u1 - u0 + 1) * (v1 - v0 + 1)) > kParallaxTiles) {
							return 1;  // zu viel Bewegung fuer eine sichere Aussage
						}
					}
				}
			}
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

		// ---------------- Hi-Z: Tiefenbild in die Kamera dieses Frames umgerechnet, als Max-Pyramide ----------------
		// Ideen aus Faster Objects (Asciim0v, GPL-3, github.com/1001Bits/Faster-Objects): Max-Mipmaps statt Kachel-Scan (jedes
		// Objekt liest hoechstens 4x4 Texel, egal wie gross), das alte Tiefenbild in die aktuelle Kamera umrechnen (weglassen
		// auch beim Drehen und Laufen statt Pause), Aufbau auf einem Hilfsthread, Matrizen in double (Weltkoordinaten bis
		// ~10^5 - in float lagen Verdeckungen dort um Prozente zu nah), Ertragsbremse und seltenere Pruefung Sichtbarer.
		using Mat4d = std::array<std::array<double, 4>, 4>;
		std::atomic<float>         g_playerFreedPct{ 0.0f };  // davon die Spielerfigur bzw. das Reittier
		// Anteil des Tiefenbilds, der wegen Figuren frei bleibt
		std::atomic<std::uint64_t> g_moverFreedTiles{ 0 }, g_moverFreedBuilds{ 0 };  // Summe je Bericht (Durchschnitt)
		std::atomic<float>         g_moverFreedMax{ 0.0f };
		std::atomic<std::uint32_t> g_moverCount{ 0 };
		std::atomic<std::uint32_t> g_actorsNear{ 0 };  // Figuren in der Naehe (davon bewegt: g_moverCount ohne Spieler und Tueren)
		std::atomic<std::uint32_t> g_doorMovers{ 0 };  // davon schwingende Tueren
		std::atomic<std::uint32_t> g_doorEvents{ 0 };  // Tuer-Ereignisse seit dem letzten Bericht

		// Tueren, die gerade aufgehen oder zugehen: die alte Tuerlage im Tiefenbild wuerde sonst noch ein paar Bilder lang
		// verdecken, was dahinter schon sichtbar ist (Review 1.0.69)
		struct DoorMove
		{
			RE::ObjectRefHandle                   handle;
			std::chrono::steady_clock::time_point until;
		};
		std::mutex            g_doorLock;
		std::vector<DoorMove> g_doors;
		constexpr std::uint32_t kGW = OcclusionGpu::kWidth, kGH = OcclusionGpu::kHeight;

		float FreedAvgPct() noexcept
		{
			const auto tiles = g_moverFreedTiles.exchange(0), builds = g_moverFreedBuilds.exchange(0);
			return builds ? 100.0f * static_cast<float>(tiles) / (static_cast<float>(builds) * kGW * kGH) : 0.0f;
		}
		constexpr float         kFree = std::numeric_limits<float>::infinity();  // keine Verdeckung bekannt

		Mat4d MatMul(const Mat4d& a_a, const Mat4d& a_b) noexcept
		{
			Mat4d r{};
			for (int i = 0; i < 4; ++i) {
				for (int j = 0; j < 4; ++j) {
					for (int k = 0; k < 4; ++k) {
						r[i][j] += a_a[i][k] * a_b[k][j];
					}
				}
			}
			return r;
		}

		bool MatInvert(const Mat4d& a_in, Mat4d& a_out) noexcept
		{
			Mat4d a = a_in;
			a_out = {};
			for (int i = 0; i < 4; ++i) {
				a_out[i][i] = 1.0;
			}
			for (int c = 0; c < 4; ++c) {
				int piv = c;
				for (int r = c + 1; r < 4; ++r) {
					if (std::abs(a[r][c]) > std::abs(a[piv][c])) {
						piv = r;
					}
				}
				if (!(std::abs(a[piv][c]) > 1e-12)) {
					return false;
				}
				std::swap(a[c], a[piv]);
				std::swap(a_out[c], a_out[piv]);
				const double inv = 1.0 / a[c][c];
				for (int j = 0; j < 4; ++j) {
					a[c][j] *= inv;
					a_out[c][j] *= inv;
				}
				for (int r = 0; r < 4; ++r) {
					if (r != c && a[r][c] != 0.0) {
						const double f = a[r][c];
						for (int j = 0; j < 4; ++j) {
							a[r][j] -= f * a[c][j];
							a_out[r][j] -= f * a_out[c][j];
						}
					}
				}
			}
			return true;
		}

		// Welt -> Clip einer gemerkten GPU-Matrix (gleiche Konvention wie Mul(CamSnap))
		Mat4d WorldToClip(const CamSnap& a_c) noexcept
		{
			Mat4d m{};
			for (int i = 0; i < 4; ++i) {
				for (int j = 0; j < 4; ++j) {
					m[i][j] = a_c.colVec ? a_c.m[i][j] : a_c.m[j][i];
				}
			}
			if (a_c.relative) {
				for (int i = 0; i < 4; ++i) {
					m[i][3] -= m[i][0] * a_c.posAdjust.x + m[i][1] * a_c.posAdjust.y + m[i][2] * a_c.posAdjust.z;
				}
			}
			return m;
		}

		struct Pyramid
		{
			std::uint32_t                   snapId = 0;
			RE::NiPoint3                    camPos{};
			RE::NiMatrix3                   camRot{};
			float                           clip[4][4]{};  // Weltpunkt minus camPos -> Clip der Kamera dieses Frames
			bool                            valid = false;  // false = nicht weglassen (Grenzen ueberschritten, keine GPU-Matrix)
			std::vector<std::vector<float>> lv;           // je Stufe: Sichttiefe der fernsten Stelle, kFree = frei
			std::vector<std::uint32_t>      w, h;
		};
		std::atomic<std::shared_ptr<const Pyramid>> g_pyr;
		std::atomic<std::uint64_t>                  g_pyrGen{ 1 };

		bool SameCamera(const RE::NiPoint3& a_p1, const RE::NiMatrix3& a_r1, const RE::NiPoint3& a_p2, const RE::NiMatrix3& a_r2) noexcept
		{
			return std::memcmp(&a_p1, &a_p2, sizeof(a_p1)) == 0 && std::memcmp(&a_r1, &a_r2, sizeof(a_r1)) == 0;
		}

		// Kamera praktisch gleich (unter 1 Einheit, unter ~0,1 Grad): die Pyramide gilt weiter. Geprueft wird ohnehin in ihrer
		// eigenen Kamera, der Unterschied liegt weit unter dem Spielraum (2 % + 16). Bis 1.0.68 nur bei exakt gleicher Lage -
		// beim langsamen Drehen blieben 4-7 % der Objekte ohne passende Pyramide ungeprueft (Review 1.0.69, L2).
		bool NearCamera(const RE::NiPoint3& a_p1, const RE::NiMatrix3& a_r1, const RE::NiPoint3& a_p2, const RE::NiMatrix3& a_r2) noexcept
		{
			const float dx = a_p1.x - a_p2.x, dy = a_p1.y - a_p2.y, dz = a_p1.z - a_p2.z;
			if (dx * dx + dy * dy + dz * dz > 1.0f) {
				return false;
			}
			for (int r = 0; r < 3; ++r) {
				for (int c = 0; c < 3; ++c) {
					if (std::fabs(a_r1.entry[r][c] - a_r2.entry[r][c]) > 0.0017f) {
						return false;
					}
				}
			}
			return true;
		}

		std::shared_ptr<const Pyramid> BuildPyramid(const Snapshot& a_s, const RE::NiPoint3& a_pos, const RE::NiMatrix3& a_rot, const RE::NiFrustum& a_fr)
		{
			// Speicher wiederverwenden (nur der Hilfsthread baut): ein frei gewordener Eintrag (nur noch hier referenziert) wird
			// neu befuellt - frisch angeforderte Vektoren kosteten bei jedem Aufbau Speicheranforderungen und Seitenfehler
			static std::vector<std::shared_ptr<Pyramid>> pool;
			std::shared_ptr<Pyramid>                     p;
			for (auto& e : pool) {
				if (e.use_count() == 1) {
					p = e;
					break;
				}
			}
			if (!p) {
				p = std::make_shared<Pyramid>();
				if (pool.size() < 4) {
					pool.push_back(p);
				}
			}
			p->valid = false;
			const auto& c = a_s.cam;
			p->snapId = a_s.id;
			p->camPos = a_pos;
			p->camRot = a_rot;
			if (!c.gpu) {
				return p;
			}
			// Grenzen der Umrechnung: stark gedreht, weit gelaufen oder anderes Sichtfeld (Zoom, Dialog) -> nichts weglassen
			const RE::NiPoint3 dir{ a_rot.entry[0][0], a_rot.entry[1][0], a_rot.entry[2][0] };
			const float        mx = a_pos.x - c.pos.x, my = a_pos.y - c.pos.y, mz = a_pos.z - c.pos.z;
			const float        tol = 0.005f * (c.r - c.l);
			if (Dot(dir, c.dir) < 0.5f || mx * mx + my * my + mz * mz > 2048.0f * 2048.0f || std::abs(a_fr.fLeft - c.l) > tol || std::abs(a_fr.fRight - c.r) > tol ||
				std::abs(a_fr.fTop - c.t) > tol || std::abs(a_fr.fBottom - c.b) > tol) {
				return p;
			}
			const Mat4d world = WorldToClip(c.gm);
			Mat4d       inv;
			if (!MatInvert(world, inv)) {
				return p;
			}
			// Punkt relativ zur neuen Kamera -> gleicher Punkt relativ zur alten: q = altPos + R_alt * R_neu^T * (p - neuPos)
			Mat4d a{};
			for (int i = 0; i < 3; ++i) {
				for (int j = 0; j < 3; ++j) {
					for (int k = 0; k < 3; ++k) {
						a[i][j] += static_cast<double>(c.rot.entry[i][k]) * a_rot.entry[j][k];
					}
				}
			}
			const double np[3]{ a_pos.x, a_pos.y, a_pos.z }, op[3]{ c.pos.x, c.pos.y, c.pos.z };
			for (int i = 0; i < 3; ++i) {
				a[i][3] = op[i] - (a[i][0] * np[0] + a[i][1] * np[1] + a[i][2] * np[2]);
			}
			a[3][3] = 1.0;
			const Mat4d cur = MatMul(world, a);  // Welt -> Clip der aktuellen Kamera
			Mat4d       eye{};
			for (int i = 0; i < 4; ++i) {
				eye[i][i] = 1.0;
			}
			eye[0][3] = a_pos.x;
			eye[1][3] = a_pos.y;
			eye[2][3] = a_pos.z;
			const Mat4d rel = MatMul(cur, eye);
			for (int i = 0; i < 4; ++i) {
				for (int j = 0; j < 4; ++j) {
					p->clip[i][j] = static_cast<float>(rel[i][j]);
				}
			}
			// Je Tiefenbild einmal (nur der Hilfsthread baut): Sichttiefe der fernsten Stelle je Kachel und ihr alter Clip-Punkt.
			// Kostete bei jedem Aufbau neu ~37000 Divisionen (1.0.68-Test: 0,5-0,6 ms je Aufbau).
			struct Source
			{
				std::uint32_t      snapId = 0;
				std::vector<float> z;                   // Sichttiefe, <= 0 = Himmel/ungueltig
				std::vector<float> pts;                 // je Kachel x*z, y*z, d*z, z
			};
			static Source src;
			if (src.snapId != a_s.id || src.z.empty()) {
				const auto& farBuf = c.reversed ? a_s.rb.minDepth : a_s.rb.maxDepth;  // fernster Wert je Kachel
				src.z.resize(static_cast<std::size_t>(kGW) * kGH);
				src.pts.resize(src.z.size() * 4);
				for (std::uint32_t v = 0; v < kGH; ++v) {
					const float y = 1.0f - (static_cast<float>(v) + 0.5f) / kGH * 2.0f;
					for (std::uint32_t u = 0; u < kGW; ++u) {
						const auto  i = static_cast<std::size_t>(v) * kGW + u;
						const float buf = farBuf[i];
						const float z = BufToZ(c, buf);
						src.z[i] = z;
						const float x = (static_cast<float>(u) + 0.5f) / kGW * 2.0f - 1.0f;
						const float d = (buf - c.depthMin) / (c.depthMax - c.depthMin);
						src.pts[i * 4 + 0] = x * z;
						src.pts[i * 4 + 1] = y * z;
						src.pts[i * 4 + 2] = d * z;
						src.pts[i * 4 + 3] = z;
					}
				}
				// Was sich mit der Kamera bewegt, wuerde umgerechnet neben seiner echten Lage als Verdeckung liegen bleiben (Review
				// 1.0.68: Schulterkamera beim seitlichen Laufen oder Reiten - ferne Objekte neben der Figur fehlten). Deshalb frei:
				// alles naeher als 128 Einheiten (Ich-Perspektive: Arme, Waffe, Zauber) und die Spielerfigur bzw. das Reittier
				// samt allem davor in ihrem Bildbereich.
				for (auto& z : src.z) {
					if (z > 0.0f && z < 128.0f) {
						z = 0.0f;
					}
				}
				std::size_t freedByMovers = 0, freedByPlayer = 0;
				const float psx = c.gpu ? c.sx : 2.0f / (c.r - c.l), psy = c.gpu ? c.sy : 2.0f / (c.t - c.b);
				for (std::size_t m = 0; m < c.movers.size(); ++m) {
					const auto& mover = c.movers[m];
					const float r = mover.radius;
					float       cc[4];
					Mul(c.gm, mover.center, cc);
					// Kugel um die Figur, je Kachel exakt geprueft: aus Bildlage und Tiefe der Kachel den Punkt in Kamerakoordinaten
					// bilden und den Abstand zur Mitte messen. Bis zum vierten Test 1.0.69 Quader bzw. Ellipsoid und bei Figuren
					// direkt an der Kamera das ganze Bild - das nahm Boden und Waende weit neben der Figur mit (bis 25-41 % des
					// Bilds frei, Verdeckung 7-15 % statt 26-33 %).
					const float cw = cc[3];
					if (cw < -r || (m == 0 && cw <= r * 1.05f + 1.0f)) {
						// ganz hinter der Kamera, oder Ich-Perspektive (der Koerper liegt nicht im Tiefenbild, Arme und Waffe deckt
						// die 128-Grenze oben ab)
						continue;
					}
					// Bildbereich: jeder Punkt liegt hoechstens r seitlich und r naeher -> |x - cx| <= r * (sx + |cx|) / (w - r);
					// reicht die Kugel bis an die Kamera, das ganze Bild (der Kugeltest unten grenzt ein)
					int u0 = 0, u1 = static_cast<int>(kGW) - 1, v0 = 0, v1 = static_cast<int>(kGH) - 1;
					if (cw > r * 1.05f + 1.0f) {
						const float cx = cc[0] / cw, cy = cc[1] / cw;
						const float hx = r * (psx + std::fabs(cx)) / (cw - r) + 1.0f / kGW, hy = r * (psy + std::fabs(cy)) / (cw - r) + 1.0f / kGH;
						if (cx - hx > 1.0f || cx + hx < -1.0f || cy - hy > 1.0f || cy + hy < -1.0f) {
							continue;  // nicht im Bild
						}
						u0 = std::max(u0, static_cast<int>(std::floor(((cx - hx) * 0.5f + 0.5f) * kGW)));
						u1 = std::min(u1, static_cast<int>(std::floor(((cx + hx) * 0.5f + 0.5f) * kGW)));
						v0 = std::max(v0, static_cast<int>(std::floor((0.5f - (cy + hy) * 0.5f) * kGH)));
						v1 = std::min(v1, static_cast<int>(std::floor((0.5f - (cy - hy) * 0.5f) * kGH)));
					}
					// Mitte in Kamerakoordinaten (clip.x = sx * x, clip.y = sy * y, clip.w = Tiefe)
					const float centerX = cc[0] / psx, centerY = cc[1] / psy;
					// halbe Kachel in Welteinheiten je Einheit Tiefe: eine Kachel, die die Kugel nur am Rand streift, zaehlt mit
					const float tileX = 1.0f / (kGW * psx), tileY = 1.0f / (kGH * psy);
					for (int v = v0; v <= v1; ++v) {
						const float y = 1.0f - (static_cast<float>(v) + 0.5f) / kGH * 2.0f;
						for (int u = u0; u <= u1; ++u) {
							auto&       z = src.z[static_cast<std::size_t>(v) * kGW + u];
							if (!(z > 0.0f) || std::fabs(z - cw) > r) {
								continue;
							}
							const float x = (static_cast<float>(u) + 0.5f) / kGW * 2.0f - 1.0f;
							const float dx = std::max(0.0f, std::fabs(x * z / psx - centerX) - z * tileX);
							const float dy = std::max(0.0f, std::fabs(y * z / psy - centerY) - z * tileY);
							const float dz = z - cw;
							if (dx * dx + dy * dy + dz * dz <= r * r) {
								z = 0.0f;
								++freedByMovers;
								if (m == 0) {
									++freedByPlayer;
								}
							}
						}
					}
				}
				g_playerFreedPct.store(100.0f * static_cast<float>(freedByPlayer) / static_cast<float>(src.z.size()), std::memory_order_relaxed);
				{
					const float pct = 100.0f * static_cast<float>(freedByMovers) / static_cast<float>(src.z.size());
					g_moverFreedTiles.fetch_add(freedByMovers, std::memory_order_relaxed);
					g_moverFreedBuilds.fetch_add(1, std::memory_order_relaxed);
					float prev = g_moverFreedMax.load(std::memory_order_relaxed);
					while (pct > prev && !g_moverFreedMax.compare_exchange_weak(prev, pct, std::memory_order_relaxed)) {
					}
				}
				g_moverCount.store(static_cast<std::uint32_t>(c.movers.size()), std::memory_order_relaxed);
				src.snapId = a_s.id;
			}
			// Stufen vorab anlegen (Referenzen bleiben gueltig)
			{
				std::size_t   levels = 1;
				std::uint32_t lw = kGW, lh = kGH;
				p->w.assign(1, lw);
				p->h.assign(1, lh);
				while (lw > 1 || lh > 1) {
					lw = (lw + 1) / 2;
					lh = (lh + 1) / 2;
					p->w.push_back(lw);
					p->h.push_back(lh);
					++levels;
				}
				p->lv.resize(levels);
				for (std::size_t i = 0; i < levels; ++i) {
					p->lv[i].resize(static_cast<std::size_t>(p->w[i]) * p->h[i]);
				}
			}
			auto& l0 = p->lv[0];
			if (SameCamera(a_pos, a_rot, c.pos, c.rot)) {
				for (std::size_t i = 0; i < l0.size(); ++i) {
					l0[i] = src.z[i] > 0.0f ? src.z[i] : kFree;
				}
			} else {
				// Kachelmitte mit fernster Tiefe vom alten in den neuen Clip-Raum; Treffer behalten den fernsten Wert
				const Mat4d k = MatMul(cur, inv);
				float       kf[4][4];
				for (int i = 0; i < 4; ++i) {
					for (int j = 0; j < 4; ++j) {
						kf[i][j] = static_cast<float>(k[i][j]);
					}
				}
				std::fill(l0.begin(), l0.end(), -1.0f);
				for (std::size_t i = 0; i < l0.size(); ++i) {
					if (!(src.z[i] > 0.0f)) {
						continue;
					}
					const float* in = &src.pts[i * 4];
					const float  cw = kf[3][0] * in[0] + kf[3][1] * in[1] + kf[3][2] * in[2] + kf[3][3] * in[3];
					if (!(cw > 16.0f)) {
						continue;
					}
					const float iw = 1.0f / cw;
					const float cx = (kf[0][0] * in[0] + kf[0][1] * in[1] + kf[0][2] * in[2] + kf[0][3] * in[3]) * iw;
					const float cy = (kf[1][0] * in[0] + kf[1][1] * in[1] + kf[1][2] * in[2] + kf[1][3] * in[3]) * iw;
					const float fu = (cx * 0.5f + 0.5f) * kGW, fv = (0.5f - cy * 0.5f) * kGH;
					if (!(fu >= 0.0f && fu < static_cast<float>(kGW) && fv >= 0.0f && fv < static_cast<float>(kGH))) {
						continue;
					}
					auto& t = l0[static_cast<std::size_t>(fv) * kGW + static_cast<std::size_t>(fu)];
					t = std::max(t, cw);
				}
				// nicht getroffen = unbekannt (frei); danach je Texel der fernste Wert der 3x3-Nachbarschaft (Luecken beim
				// Naeherkommen, Versatz innerhalb einer Kachel)
				for (auto& t : l0) {
					if (t < 0.0f) {
						t = kFree;
					}
				}
				static std::vector<float> tmp;
				tmp.resize(l0.size());
				for (std::uint32_t v = 0; v < kGH; ++v) {
					const auto row = static_cast<std::size_t>(v) * kGW;
					for (std::uint32_t u = 0; u < kGW; ++u) {
						float m = l0[row + u];
						if (u > 0) {
							m = std::max(m, l0[row + u - 1]);
						}
						if (u + 1 < kGW) {
							m = std::max(m, l0[row + u + 1]);
						}
						tmp[row + u] = m;
					}
				}
				for (std::uint32_t v = 0; v < kGH; ++v) {
					for (std::uint32_t u = 0; u < kGW; ++u) {
						float m = tmp[static_cast<std::size_t>(v) * kGW + u];
						if (v > 0) {
							m = std::max(m, tmp[static_cast<std::size_t>(v - 1) * kGW + u]);
						}
						if (v + 1 < kGH) {
							m = std::max(m, tmp[static_cast<std::size_t>(v + 1) * kGW + u]);
						}
						l0[static_cast<std::size_t>(v) * kGW + u] = m;
					}
				}
			}
			// Max-Mipmaps: jede Stufe haelt die fernste Tiefe ihrer (bis zu) 2x2 Texel der Stufe darunter
			for (std::size_t lvl = 1; lvl < p->lv.size(); ++lvl) {
				const std::uint32_t w = p->w[lvl - 1], h = p->h[lvl - 1], nw = p->w[lvl], nh = p->h[lvl];
				const auto&         prev = p->lv[lvl - 1];
				auto&               next = p->lv[lvl];
				for (std::uint32_t y = 0; y < nh; ++y) {
					for (std::uint32_t x = 0; x < nw; ++x) {
						const std::uint32_t x0 = 2 * x, y0 = 2 * y, x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
						next[static_cast<std::size_t>(y) * nw + x] = std::max(std::max(prev[static_cast<std::size_t>(y0) * w + x0], prev[static_cast<std::size_t>(y0) * w + x1]),
							std::max(prev[static_cast<std::size_t>(y1) * w + x0], prev[static_cast<std::size_t>(y1) * w + x1]));
					}
				}
			}
			p->valid = true;
			return p;
		}

		// 1 = sichtbar, 2 = verdeckt (wie Test)
		int TestPyramid(const Pyramid& a_p, const RE::NiBound& a_b) noexcept
		{
			if (!a_p.valid) {
				return 1;
			}
			const auto proj = [&](float a_x, float a_y, float a_z, float& a_nx, float& a_ny, float& a_w) {
				const auto& m = a_p.clip;
				a_w = m[3][0] * a_x + m[3][1] * a_y + m[3][2] * a_z + m[3][3];
				const float iw = a_w != 0.0f ? 1.0f / a_w : 0.0f;
				a_nx = (m[0][0] * a_x + m[0][1] * a_y + m[0][2] * a_z + m[0][3]) * iw;
				a_ny = (m[1][0] * a_x + m[1][1] * a_y + m[1][2] * a_z + m[1][3]) * iw;
			};
			const float dx = a_b.center.x - a_p.camPos.x, dy = a_b.center.y - a_p.camPos.y, dz = a_b.center.z - a_p.camPos.z;
			const float rad = std::max(a_b.radius, 0.0f);
			float       nx, ny, z;
			proj(dx, dy, dz, nx, ny, z);
			const float wn = z - rad;
			if (!(wn > 16.0f)) {
				return 1;
			}
			float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
			for (int i = 0; i < 8; ++i) {
				float px, py, pw;
				proj(dx + ((i & 1) ? rad : -rad), dy + ((i & 2) ? rad : -rad), dz + ((i & 4) ? rad : -rad), px, py, pw);
				if (!(pw > 1.0f)) {
					return 1;
				}
				minX = std::min(minX, px);
				maxX = std::max(maxX, px);
				minY = std::min(minY, py);
				maxY = std::max(maxY, py);
			}
			constexpr int W = static_cast<int>(kGW), H = static_cast<int>(kGH);
			int           u0 = static_cast<int>(std::floor((minX * 0.5f + 0.5f) * W)) - 1;
			int           u1 = static_cast<int>(std::floor((maxX * 0.5f + 0.5f) * W)) + 1;
			int           v0 = static_cast<int>(std::floor((0.5f - maxY * 0.5f) * H)) - 1;
			int           v1 = static_cast<int>(std::floor((0.5f - minY * 0.5f) * H)) + 1;
			if (u1 < 0 || v1 < 0 || u0 > W - 1 || v0 > H - 1) {
				return 1;  // ausserhalb des Bilds - zeichnet das Spiel ohnehin nicht
			}
			// Die Pyramide gilt fuer die Kamera dieses Frames: Teile ausserhalb des Bilds sind nicht zu sehen
			u0 = std::max(u0, 0);
			v0 = std::max(v0, 0);
			u1 = std::min(u1, W - 1);
			v1 = std::min(v1, H - 1);
			std::size_t lvl = 0;
			while (lvl + 1 < a_p.lv.size() && (((u1 >> lvl) - (u0 >> lvl)) > 3 || ((v1 >> lvl) - (v0 >> lvl)) > 3)) {
				++lvl;
			}
			const float marginK = 1.0f + Config::occlMarginPct.load(std::memory_order_relaxed) * 0.01f;
			const float marginU = Config::occlMarginUnits.load(std::memory_order_relaxed);
			const auto& t = a_p.lv[lvl];
			const auto  tw = a_p.w[lvl];
			for (int v = v0 >> lvl; v <= (v1 >> lvl); ++v) {
				for (int u = u0 >> lvl; u <= (u1 >> lvl); ++u) {
					// fernste Verdeckung des Bereichs; die naechste Stelle der Kugel muss klar dahinter liegen (frei = sichtbar)
					if (!(wn > t[static_cast<std::size_t>(v) * tw + u] * marginK + marginU)) {
						return 1;
					}
				}
			}
			return 2;
		}

		// Aufbau auf einem Hilfsthread: angestossen, sobald die Kamera des Frames feststeht (Sonnenschatten-Kamera) oder
		// spaetestens vom ersten Objekt des Frames. Bis die Pyramide zur Kamera passt, wird nichts weggelassen.
		struct BuildRequest
		{
			std::shared_ptr<const Snapshot> snap;
			RE::NiPoint3                    pos{};
			RE::NiMatrix3                   rot{};
			RE::NiFrustum                   fr{};
			std::uint32_t                   resetGen = 0;
		};
		std::atomic<std::uint32_t>  g_resetGen{ 0 };  // Ladebildschirm: angefangene Aufbauten verwerfen
		std::mutex                  g_buildLock;
		std::condition_variable     g_buildCv;
		std::optional<BuildRequest> g_buildReq;
		bool                        g_builderStarted = false;
		std::atomic<std::uint64_t>  g_buildKey{ 0 };  // zuletzt angefordert (Kamera + Tiefenbild)
		std::atomic<std::uint64_t>  g_buildNs{ 0 }, g_builds{ 0 }, g_notReady{ 0 }, g_kickBegin{ 0 }, g_kickLate{ 0 };

		std::uint64_t CameraKey(const RE::NiPoint3& a_pos, const RE::NiMatrix3& a_rot, std::uint32_t a_snapId) noexcept
		{
			std::uint64_t h = 0xCBF29CE484222325ull ^ a_snapId;
			const auto    mix = [&](const void* a_p, std::size_t a_n) {
                const auto* b = static_cast<const std::uint8_t*>(a_p);
                for (std::size_t i = 0; i < a_n; ++i) {
                    h = (h ^ b[i]) * 0x100000001B3ull;
                }
			};
			mix(&a_pos, sizeof(a_pos));
			mix(&a_rot, sizeof(a_rot));
			return h;
		}

		void Builder()
		{
#ifdef TRACY_ENABLE
			tracy::SetThreadName("SPS Verdeckung");
#endif
			for (;;) {
				BuildRequest r;
				{
					std::unique_lock l(g_buildLock);
					g_buildCv.wait(l, [] { return g_buildReq.has_value(); });
					r = std::move(*g_buildReq);
					g_buildReq.reset();
				}
				const auto cur = g_pyr.load();
				if (cur && cur->snapId == r.snap->id && NearCamera(cur->camPos, cur->camRot, r.pos, r.rot)) {
					continue;
				}
				const auto t0 = Clock::now();
				try {
					ZoneScopedN("Verdeckung: Pyramide bauen");
					auto p = BuildPyramid(*r.snap, r.pos, r.rot, r.fr);
					if (r.resetGen != g_resetGen.load(std::memory_order_acquire)) {
						continue;  // Welt inzwischen gewechselt
					}
					g_pyr.store(std::move(p));
					g_pyrGen.fetch_add(1, std::memory_order_release);
				} catch (...) {
					continue;  // kein Speicher: ohne Pyramide wird nichts weggelassen
				}
				g_buildNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count()), std::memory_order_relaxed);
				g_builds.fetch_add(1, std::memory_order_relaxed);
			}
		}

		// Fuer die Weltkamera (wie sie jetzt steht) und das neueste Tiefenbild anfordern - doppelte Anforderungen fallen weg
		// a_snapId: Tiefenbild, das der Aufrufer kennt (0 = selbst holen). Erst nach der Doppel-Pruefung wird der shared_ptr
		// geladen - er kostet eine Sperre und wird sonst je Objekt geholt (bis 1.0.57 genau dieses Problem).
		void RequestPyramid(std::uint32_t a_snapId = 0) noexcept
		{
			const auto cam = RE::Main::WorldRootCamera();
			if (!cam) {
				return;
			}
			const auto&                     w = cam->world;
			std::shared_ptr<const Snapshot> snap;
			if (!a_snapId) {
				snap = g_current.load();
				if (!snap) {
					return;
				}
				a_snapId = snap->id;
			}
			const auto key = CameraKey(w.translate, w.rotate, a_snapId);
			if (g_buildKey.load(std::memory_order_relaxed) == key || g_buildKey.exchange(key, std::memory_order_relaxed) == key) {
				return;
			}
			if (!snap) {
				snap = g_current.load();
				if (!snap) {
					return;
				}
			}
			try {
				std::scoped_lock l(g_buildLock);
				if (!g_builderStarted) {
					std::thread(Builder).detach();
					g_builderStarted = true;
				}
				g_buildReq = BuildRequest{ std::move(snap), w.translate, w.rotate, cam->GetRuntimeData2().viewFrustum, g_resetGen.load(std::memory_order_relaxed) };
			} catch (...) {
				return;
			}
			g_buildCv.notify_one();
		}

		// Pyramide fuer diesen Thread; nullptr, wenn sie nicht zur Kamera dieses Frames passt (dann neu anfordern)
		const Pyramid* PyramidForThread(const RE::NiPoint3& a_pos, const RE::NiMatrix3& a_rot) noexcept
		{
			thread_local std::shared_ptr<const Pyramid> pyr;
			thread_local std::uint64_t                  gen = 0;
			const auto                                  now = g_pyrGen.load(std::memory_order_acquire);
			if (now != gen) {
				pyr = g_pyr.load();
				gen = now;
			}
			if (!pyr || !NearCamera(pyr->camPos, pyr->camRot, a_pos, a_rot)) {
				return nullptr;
			}
			return pyr.get();
		}

		// ---------------- Ertragsbremse und seltenere Pruefung sichtbarer Objekte (ebenfalls nach Faster Objects) ----------
		// Laesst das Pruefen kaum etwas weg (unter 5 % der Pruefungen ueber 120 Frames), wird nur noch in 4 von 32 Frames
		// geprueft; ein Frame, der genug weglaesst, schaltet zurueck auf jeden Frame. Kostet sonst ohne Nutzen (Weisslauf-Markt).
		struct YieldGate
		{
			static constexpr float         kMinYield = 0.05f;
			static constexpr std::uint32_t kInterval = 32, kBurst = 4, kLowFrames = 120, kMinTests = 64;
			std::uint32_t                  low = 0, frame = 0;
			bool                           Step(std::uint32_t a_tested, std::uint32_t a_culled) noexcept
			{
				++frame;
				if (a_tested >= kMinTests) {
					if (static_cast<float>(a_culled) >= kMinYield * static_cast<float>(a_tested)) {
						low = 0;
					} else if (low < kLowFrames) {
						++low;
					}
				}
				return low < kLowFrames || frame % kInterval < kBurst;
			}
		};
		YieldGate                  g_gate;               // Main-Thread
		std::atomic<bool>          g_testFrame{ true };  // in diesem Frame pruefen
		std::atomic<std::uint32_t> g_frameNo{ 1 };       // Frame-Zaehler fuer die Sammel-Threads
		std::atomic<std::uint32_t> g_yieldTested{ 0 }, g_yieldCulled{ 0 };
		std::uint64_t              g_gatedFrames = 0;  // Main-Thread, fuer den Bericht
		std::atomic<std::uint64_t> g_visSkipped{ 0 };
		std::atomic<bool>          g_frameBeginHooked{ false };  // Main::Begin zaehlt die Bilder (sonst Present)

		// Neues Bild: Merkliste des Draw-Filters, Frame-Zaehler der Sammel-Threads, Ertragsbremse. Am Main::Begin des Spiels -
		// haengte nur der Present-Hook daran und fiele weg (Overlay, Frame Generation baut die Swapchain neu), blieben einmal
		// markierte Objekte dauerhaft unsichtbar (Review 1.0.69).
		void AdvanceFrame() noexcept
		{
			g_hiddenStamp.fetch_add(1, std::memory_order_relaxed);
			g_frameNo.fetch_add(1, std::memory_order_relaxed);
			const bool test = g_gate.Step(g_yieldTested.exchange(0, std::memory_order_relaxed), g_yieldCulled.exchange(0, std::memory_order_relaxed));
			g_gatedFrames += !test;
			g_testFrame.store(test, std::memory_order_relaxed);
		}

		// Zaehler je Thread sammeln, nur gelegentlich auf die gemeinsamen Zaehler (sonst eine Cache-Zeile fuer ~2000 Objekte)
		void CountYield(bool a_culled) noexcept
		{
			thread_local std::uint32_t frame = 0, tested = 0, culled = 0;
			const auto                 now = g_frameNo.load(std::memory_order_relaxed);
			if (now != frame || tested >= 32) {
				if (tested) {
					g_yieldTested.fetch_add(tested, std::memory_order_relaxed);
					g_yieldCulled.fetch_add(culled, std::memory_order_relaxed);
				}
				frame = now;
				tested = culled = 0;
			}
			++tested;
			culled += a_culled;
		}

		// Sichtbar gefundene Objekte erst nach 1 bis 8 Frames wieder pruefen (feste Phase je Objekt, verteilt die Pruefungen).
		// Zeichnen ist immer richtig: eine ausgelassene Pruefung kostet nur Weglassen, nie ein fehlendes Objekt. Ein Wort je
		// Platz (Schluessel | Frame, bis zu dem uebersprungen wird), ohne Sperre; Kollisionen pruefen nur oefter.
		constexpr std::size_t                             kVisSlots = 1u << 14;
		std::array<std::atomic<std::uint64_t>, kVisSlots> g_vis{};

		std::uint64_t VisHash(const void* a_p) noexcept
		{
			auto h = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(a_p));
			h ^= h >> 33;
			h *= 0xFF51AFD7ED558CCDull;
			h ^= h >> 33;
			return h;
		}

		bool VisibleSkip(const void* a_geom, std::uint32_t a_frame) noexcept
		{
			const auto h = VisHash(a_geom);
			const auto e = g_vis[h & (kVisSlots - 1)].load(std::memory_order_relaxed);
			return (e >> 32) == (h >> 32) && static_cast<std::int32_t>(static_cast<std::uint32_t>(e) - a_frame) > 0;
		}

		void VisibleMark(const void* a_geom, std::uint32_t a_frame) noexcept
		{
			const auto h = VisHash(a_geom);
			const auto until = a_frame + 2 + static_cast<std::uint32_t>((h >> 32) % 8);  // uebersprungen: Frames a_frame+1 .. until-1
			g_vis[h & (kVisSlots - 1)].store(((h >> 32) << 32) | until, std::memory_order_relaxed);
		}

		// ---------------- Bottled Shaders (Community-Shaders-Fork, gleiche CommunityShaders.dll) ----------------
		// Eigene "HiZ Occlusion Culling": installiert, wenn Shaders\Features\HiZOcclusion.ini existiert (gibt es nur bei
		// Bottled), aktiv, solange nicht unter "Disable at Boot" und enableHiZCulling nicht false (SettingsUser ueber
		// SettingsDefault). Ihr Weglassen greift nur auf AE vor 1.7.99. Erkennung wie bei Faster Objects.
		std::atomic<bool> g_bottledHiZ{ false };
		std::atomic<bool> g_bottledInstalled{ false };  // Bottled Shaders geladen (unabhaengig von seiner HiZ-Verdeckung)

		std::optional<bool> JsonBool(const std::string& a_text, const char* a_key)
		{
			const std::regex re(std::string("\"") + a_key + "\"\\s*:\\s*(true|false)");
			std::smatch      m;
			if (std::regex_search(a_text, m, re)) {
				return m[1] == "true";
			}
			return std::nullopt;
		}

		bool BottledBlocks() noexcept
		{
			return g_bottledHiZ.load(std::memory_order_relaxed) && !Config::occlAllowWithBottled.load(std::memory_order_relaxed);
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
			return Config::occlusionProbe.load(std::memory_order_relaxed) || (Config::occlusionCull.load(std::memory_order_relaxed) && Config::masterEnabled.load(std::memory_order_relaxed) &&
			                                                                      !BottledBlocks() && ShadowCulling::MainDrawFilterReady());
		}

		// Ausgewerteter Bereich des Tiefenpuffers: Viewport des Vorpasses; ist der so gross wie der ganze Puffer, die
		// Engine aber auf dynamischer Aufloesung, deren Anteil (links oben)
		float         g_region[4]{};
		const float*  Region(const OwnCam& a_c) noexcept
		{
			std::copy(std::begin(a_c.vp), std::end(a_c.vp), g_region);
			const bool fullVp = !(a_c.vp[2] > 0.0f) || (g_lastSrcW && a_c.vp[2] >= g_lastSrcW - 1.0f);
			if (fullVp && g_lastSrcW && g_lastSrcH && (a_c.resW < 0.99f || a_c.resH < 0.99f)) {
				g_region[0] = g_region[1] = 0.0f;
				g_region[2] = a_c.resW * g_lastSrcW;
				g_region[3] = a_c.resH * g_lastSrcH;
			}
			return g_region;
		}

		// Zaehler, Zeitmessung und Minutenbericht nur beim Messen (Debug) oder mit Analyse-Protokoll
		bool Diag() noexcept
		{
			return Config::occlusionProbe.load(std::memory_order_relaxed) || Config::analysis.load(std::memory_order_relaxed);
		}

		void Report()
		{
			const auto now = Clock::now();
			if (now - g_reportStart < std::chrono::seconds(60)) {
				return;
			}
			const bool   log = Diag();
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
			if (log) logger::info("[Occlusion] {:.0f} s, {:.0f} frames | prepass {} | camera matrix {} usable {} frames, not {} (avg w error {:.0f}) | GPU matrix at prepass start {} ok / {} not, depth range {:.4f}-{:.4f} | own projection: right {:+.0f} up {:+.0f} (compared {} frames), depth {} A {:.5f} B {:.3f}{}, max NDC diff {:.3f} | depth source {} ({}x{}, used {}x{} at {},{}, viewport {:.0f}x{:.0f}, dyn. res. {:.2f}x{:.2f}) | captures {}, ring full {}, no depth {} | reads {}, latency {:.1f} frames | sky tiles {:.0f} % | test time {:.2f} ms/frame (all threads) | no snapshot yet {}",
				secs, frames, g_prepassCalls, g_camLayout, g_learnFrames, g_camFail, g_camFail ? g_camFailErrSum / g_camFail : 0.0f, g_beginOk, g_beginFail, g_beginDepthMin, g_beginDepthMax, g_rightSign, g_upSign, g_gpuSignFrames,
				g_learned ? "learned" : "formula", g_learnA, g_learnB, g_learnRev ? " reversed" : "", g_learnMaxErr, g_lastSource == 0 ? "post-prepass copy" : "main",
				g_lastSrcW, g_lastSrcH, cur ? cur->rb.regW : 0, cur ? cur->rb.regH : 0, cur ? cur->rb.regX : 0, cur ? cur->rb.regY : 0, cur ? cur->cam.vp[2] : 0.0f, cur ? cur->cam.vp[3] : 0.0f, cur ? cur->cam.resW : 0.0f, cur ? cur->cam.resH : 0.0f, g_captures, g_ringFull, g_noSrv, g_reads, g_reads ? static_cast<double>(g_latencySum) / g_reads : 0.0,
				cur ? 100.0 * sky / cur->rb.maxDepth.size() : 0.0, g_testNs.exchange(0) / 1e6 / frames, g_noSnapshot.exchange(0));
			for (auto& s : g_acc) {
				const auto geoms = s.geoms.exchange(0);
				const auto acc = s.acc.exchange(nullptr);
				const auto draws = s.draws.exchange(0), tested = s.tested.exchange(0), occGeoms = s.occGeoms.exchange(0), occDraws = s.occDraws.exchange(0);
				const auto culled = s.culledDraws.exchange(0);
				const auto tooBig = s.tooBig.exchange(0), occSkinned = s.occSkinned.exchange(0), occSmall = s.occSmall.exchange(0), occMid = s.occMid.exchange(0), occLarge = s.occLarge.exchange(0);
				if (!log || !acc || !geoms) {
					continue;
				}
				logger::info("[Occlusion]   accumulator {} | per frame: objects {:.0f}, draws {:.0f}, tested {:.0f} | occluded: objects {:.0f} ({:.0f} % of tested), draws {:.0f} ({:.0f} % of all) | occluded draws by radius <50 {:.0f}, <200 {:.0f}, larger {:.0f}, skinned {:.0f} | too big {:.0f} | culled draws {:.0f}{}",
					acc, geoms / frames, draws / frames, tested / frames, occGeoms / frames, tested ? 100.0 * occGeoms / tested : 0.0, occDraws / frames,
					draws ? 100.0 * occDraws / draws : 0.0, occSmall / frames, occMid / frames, occLarge / frames, occSkinned / frames, tooBig / frames, culled / frames, acc == g_mainAcc.load() ? " (main view)" : "");
			}
			g_accOther = 0;
			g_prepassCalls = g_camFail = g_captures = g_ringFull = g_noSrv = g_reads = g_latencySum = 0;
			if (log) logger::info("[Occlusion]   hiding occluded objects: {} | frames without hiding because the camera moved: {} | settings: pause from {:.1f} deg / {:.0f} units, margin {:.1f} % + {:.0f}, hidden in {} images in a row, max {} tiles, characters {}", Config::occlusionCull.load() && Config::masterEnabled.load() ? "ON" : "off", g_cullBlockedFrames, Config::occlTurnDeg.load(), Config::occlMoveUnits.load(), Config::occlMarginPct.load(), Config::occlMarginUnits.load(), Config::occlStreak.load(), Config::occlMaxTiles.load(), Config::occlSkipSkinned.load() ? "never" : "allowed");
			g_cullBlockedFrames = 0;
			{
				const auto builds = g_builds.exchange(0), ns = g_buildNs.exchange(0);
				if (log) logger::info("[Occlusion]   reprojection {} | pyramid builds {} ({:.2f} ms each, helper thread) | objects without a matching pyramid {:.0f}/frame (late requests {}, requests at frame start {}) | frames skipped by the yield gate {} | visible objects not re-tested {:.0f}/frame | moving figures {} of {}, doors {} ({} door events) free {:.1f} % of the depth image on average, max {:.1f} % (player now {:.1f} %) | Bottled Shaders HiZ {}",
					Config::occlReproject.load() ? "on" : "off", builds, builds ? ns / 1e6 / builds : 0.0, g_notReady.exchange(0) / frames, g_kickLate.exchange(0), g_kickBegin.exchange(0), g_gatedFrames, g_visSkipped.exchange(0) / frames,
					g_moverCount.load() > g_doorMovers.load() ? g_moverCount.load() - g_doorMovers.load() - 1 : 0, g_actorsNear.load(), g_doorMovers.load(), g_doorEvents.exchange(0), FreedAvgPct(), g_moverFreedMax.exchange(0.0f), g_playerFreedPct.load(), g_bottledHiZ.load() ? (Config::occlAllowWithBottled.load() ? "active, SPS allowed anyway" : "active, SPS hiding off") : "not active");
				g_gatedFrames = 0;
				if (log && g_poseMismatch) {
					logger::info("[Occlusion]   depth images not used because camera pose and GPU matrix differed: {}", g_poseMismatch);
				}
				g_poseMismatch = 0;
			}
			if (log) logger::info("[Occlusion]   hiding mode: {} | marked {:.0f}/frame, draws left out at draw time {:.0f}/frame, table full {}",
				Config::occlDrawFilter.load() ? "draw filter" : "collection (old)", g_hiddenMarked.exchange(0) / frames, g_filterSkipped.exchange(0) / frames,
				g_hiddenFull.exchange(0));
			{
				std::scoped_lock l(g_sampleLock);
				for (const auto& smp : g_samples) {
					if (log) logger::info("[Occlusion]     hidden: {}", smp);
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
		ZoneScopedN("Verdeckung: Tiefenbild + Figuren");
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
		own.rot = rot;
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
		if (const auto renderer = RE::BSGraphics::Renderer::GetSingleton()) {
			OcclusionGpu::CurrentViewport(renderer->GetRuntimeData().context, own.vp);
		}
		if (const auto gs = RE::BSGraphics::State::GetSingleton()) {
			own.resW = std::clamp(gs->GetRuntimeData().dynamicResolutionWidthRatio, 0.1f, 1.0f);
			own.resH = std::clamp(gs->GetRuntimeData().dynamicResolutionHeightRatio, 0.1f, 1.0f);
		}
		// Was sich bewegt, darf nach dem Umrechnen nicht als Verdeckung an der alten Stelle liegen bleiben: die Spielerfigur
		// (bewegt sich mit der Kamera) und Figuren, die laufen oder sich drehen (NPCs, Pferde, Drachen - Review 1.0.69: hinter
		// einem vorbeilaufenden NPC fehlten fuer 1-3 Bilder kleine Objekte). Stehende Figuren bleiben Verdeckung: sie stehen im
		// alten Tiefenbild an der richtigen Stelle - alle Figuren freizugeben kostete am Markt 16-41 % des Bilds (Test 1.0.69).
		struct ActorTrack
		{
			RE::NiPoint3  pos;
			float         angle = 0.0f;
			std::uint64_t moved = 0, seen = 0;
		};
		static std::unordered_map<RE::FormID, ActorTrack> s_tracks;
		static std::uint64_t                               s_tick = 0;
		constexpr std::uint64_t                            kMovedFrames = 12;  // Tiefenbild ist einige Bilder alt + Streak
		++s_tick;
		std::uint32_t actorsNear = 0;
		own.movers.clear();
		if (const auto player = RE::PlayerCharacter::GetSingleton()) {
			const auto pp = player->GetPosition();
			const bool mounted = player->IsOnMount();
			own.movers.push_back({ { pp.x, pp.y, pp.z + (mounted ? 128.0f : 64.0f) }, mounted ? 256.0f : 128.0f });
		}
		if (const auto lists = RE::ProcessLists::GetSingleton()) {
			for (const auto& handle : lists->highActorHandles) {
				if (own.movers.size() >= 160) {
					break;
				}
				const auto actor = handle.get();
				if (!actor || !actor->Is3DLoaded()) {
					continue;
				}
				++actorsNear;
				{
					const auto  p = actor->GetPosition();
					const float a = actor->GetAngleZ();
					auto [it, inserted] = s_tracks.try_emplace(actor->GetFormID(), ActorTrack{ p, a, s_tick, s_tick });
					auto& t = it->second;
					if (!inserted) {
						// gegen die Lage beim letzten erkannten Bewegen, nicht gegen das letzte Bild: sonst rutscht langsames
						// Schleichen (< 1 Einheit pro Bild) dauerhaft unter der Schwelle durch
						float da = std::fabs(a - t.angle);
						da = std::min(da, 6.2831853f - da);
						if (p.GetSquaredDistance(t.pos) > 4.0f || da > 0.02f) {
							t.moved = s_tick;
							t.pos = p;
							t.angle = a;
						}
						t.seen = s_tick;
					}
					// Kampf (Schwertschwuenge, Ausweichen) und Umfallen/Aufstehen aendern die Gestalt, ohne dass sich die Lage
					// der Figur nennenswert aendert
					const bool animating = actor->IsInCombat() || actor->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal;
					if (!animating && s_tick - t.moved > kMovedFrames) {
						continue;  // steht still: bleibt Verdeckung
					}
				}
				// Groesse aus den Grundmassen der Figur (wie im Creation Kit), nicht aus der Huelle des 3D-Modells: die ist bei
				// Figuren oft viel zu gross (Faster Objects: Skelett 4605, Irrlichter 11254 Einheiten) - im Test 1.0.69 gab sie
				// am Markt fast das ganze Tiefenbild frei, die Verdeckung liess nichts mehr weg.
				const auto pos = actor->GetPosition();
				const auto bmin = actor->GetBoundMin(), bmax = actor->GetBoundMax();
				const float ex = bmax.x - bmin.x, ey = bmax.y - bmin.y, ez = bmax.z - bmin.z;
				float      r = 0.5f * std::sqrt(ex * ex + ey * ey + ez * ez);
				if (!(r > 16.0f && r < 1024.0f)) {
					r = 96.0f;  // keine brauchbaren Masse: Mensch
				}
				const float midZ = (bmin.z + bmax.z) > 0.0f && (bmin.z + bmax.z) < 4096.0f ? 0.5f * (bmin.z + bmax.z) : 64.0f;
				own.movers.push_back({ { pos.x, pos.y, pos.z + midZ }, r * 1.2f + 32.0f });  // Spielraum: Animation, Waffen
			}
		}
		if (s_tick % 600 == 0) {
			std::erase_if(s_tracks, [](const auto& e) { return s_tick - e.second.seen > 600; });
		}
		{
			std::uint32_t doors = 0;
			std::lock_guard lock(g_doorLock);
			const auto      now = std::chrono::steady_clock::now();
			std::erase_if(g_doors, [&](const DoorMove& d) { return d.until < now; });
			for (const auto& d : g_doors) {
				const auto ref = d.handle.get();
				const auto obj = ref ? ref->Get3D() : nullptr;
				if (obj && obj->worldBound.radius > 0.0f && obj->worldBound.radius < 2048.0f && own.movers.size() < 192) {
					own.movers.push_back({ obj->worldBound.center, obj->worldBound.radius * 1.1f + 16.0f });
					++doors;
				}
			}
			g_doorMovers.store(doors, std::memory_order_relaxed);
		}
		g_actorsNear.store(actorsNear, std::memory_order_relaxed);

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
			if (!allowed && !Config::occlReproject.load(std::memory_order_relaxed)) {
				++g_cullBlockedFrames;  // mit Umrechnung pausiert nichts
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
			const float err = std::max(std::abs(rc[0] / rc[3] - ox), std::abs(rc[1] / rc[3] - oy));
			g_learnMaxErr = std::max(g_learnMaxErr, err);
			++g_gpuSignFrames;
			// Umrechnung und Test setzen voraus, dass Kamera-Lage (Ende des Vorpasses) und GPU-Matrix (Beginn) zum selben Blick
			// gehoeren - sonst verschoeben sich alle Verdeckungen. Im Test immer < 0,0005; deutlich mehr: Bild nicht benutzen.
			if (!(err < 0.002f)) {
				own.gpu = false;
				++g_poseMismatch;
			}
		}
	}
	void MarkHidden(const RE::BSGeometry* a_geom) noexcept
	{
		const auto stamp = g_hiddenStamp.load(std::memory_order_relaxed);
		const auto h = HiddenHash(a_geom);
		for (std::size_t i = 0; i < kHiddenProbe; ++i) {
			auto&      s = g_hidden[(h + i) & (kHiddenSlots - 1)];
			const auto key = s.key.load(std::memory_order_relaxed);
			if (key == a_geom) {
				s.stamp.store(stamp, std::memory_order_release);
				g_markedStamp.store(stamp, std::memory_order_release);
				if (Diag()) {
					g_hiddenMarked.fetch_add(1, std::memory_order_relaxed);
				}
				return;
			}
			if (s.stamp.load(std::memory_order_relaxed) != stamp) {
				auto expected = key;
				if (s.key.compare_exchange_strong(expected, a_geom, std::memory_order_relaxed)) {
					s.stamp.store(stamp, std::memory_order_release);
					g_markedStamp.store(stamp, std::memory_order_release);
					if (Diag()) {
						g_hiddenMarked.fetch_add(1, std::memory_order_relaxed);
					}
					return;
				}
			}
		}
		g_hiddenFull.fetch_add(1, std::memory_order_relaxed);  // Tabelle voll: wird gezeichnet
	}

	// Laeuft fuer jeden Draw des Hauptbilds und des Tiefenvorpasses (Main-Thread). Bis 1.0.68 lief die Suche bei jedem nicht
	// markierten Objekt ueber alle 32 Plaetze - auch mit ausgeschalteter Verdeckung (Review: geschaetzt 0,1-0,5 ms je Frame).
	bool IsHidden(const RE::BSGeometry* a_geom) noexcept
	{
		const auto stamp = g_hiddenStamp.load(std::memory_order_relaxed);
		if (g_markedStamp.load(std::memory_order_acquire) != stamp) {
			return false;  // in diesem Bild nichts markiert
		}
		const auto h = HiddenHash(a_geom);
		for (std::size_t i = 0; i < kHiddenProbe; ++i) {
			const auto& s = g_hidden[(h + i) & (kHiddenSlots - 1)];
			if (s.key.load(std::memory_order_relaxed) == a_geom) {
				return s.stamp.load(std::memory_order_acquire) == stamp;
			}
			// MarkHidden belegt den ersten Platz mit altem Bildzaehler: dahinter kann das Objekt in diesem Bild nicht stehen
			if (s.stamp.load(std::memory_order_relaxed) != stamp) {
				return false;
			}
		}
		return false;
	}

	void CountFilterSkip() noexcept
	{
		if (Diag()) {
			g_filterSkipped.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void OnPresent() noexcept
	{
		ZoneScopedN("Verdeckung: Present");
		++g_frame;
		if (!g_frameBeginHooked.load(std::memory_order_relaxed)) {
			AdvanceFrame();  // ohne Main::Begin-Hook: wie bis 1.0.68 am Present
		}
		// Tracy-/Log-Marke bei jeder Aenderung von Schalter/Stellschrauben (Abschnitte einer Aufnahme zuordnen), nur mit Analyse-Protokoll
		if (Config::analysis.load(std::memory_order_relaxed)) {
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
			if (g_current.load()) {
				Publish(nullptr);
			}
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
			Publish(std::move(newest));
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
			} else if (OcclusionGpu::Capture(ctx, srv, g_nextTag, Region(g_snaps[g_nextTag % kSnaps]))) {
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
		// Weglassen nur ueber den Draw-Filter: fehlt dessen Hook (andere Version, fremde Mod), bleibt die Verdeckung aus - der
		// alte Weg (leere Pass-Liste) steht unter Verdacht fuer Licht-Abstuerze, ohne Filter kostete das Pruefen nur (Review)
		const bool cullOn = Config::occlusionCull.load(std::memory_order_relaxed) && Config::masterEnabled.load(std::memory_order_relaxed) && !BottledBlocks() &&
		                    ShadowCulling::MainDrawFilterReady();
		const bool probe = Config::occlusionProbe.load(std::memory_order_relaxed);
		if (!probe && !cullOn) {
			return false;
		}
		const bool diag = Diag();
		auto&      s = Slot(a_accumulator);
		// Hauptbild erkennen (wird auch zum Weglassen gebraucht) - Stichprobe je Thread genuegt fuer die Mehrheit
		thread_local std::uint32_t tick = 0;
		if ((++tick & 15) == 0) {
			s.window.fetch_add(1, std::memory_order_relaxed);
		}
		if (diag) {
			s.geoms.fetch_add(1, std::memory_order_relaxed);
			s.draws.fetch_add(a_draws, std::memory_order_relaxed);
		}
		const auto snap = CurrentForThread();
		if (!snap) {
			if (diag) {
				g_noSnapshot.fetch_add(1, std::memory_order_relaxed);
			}
			return false;
		}
		const bool skinned = const_cast<RE::BSGeometry&>(a_geom).GetGeometryRuntimeData().skinInstance != nullptr;
		const bool isMain = ShadowCulling::IsMainViewAccumulator(a_accumulator);  // gleiche Erkennung wie Mikro-/Decal-Culling
		// Ohne Diagnose zaehlt das Ergebnis nur fuer feste Objekte im Hauptbild - Spiegelungen, Vorschauen und Figuren nicht
		// erst testen (bis zu occlMaxTiles Kacheln je Objekt)
		if (!diag && (!cullOn || !isMain || (skinned && Config::occlSkipSkinned.load(std::memory_order_relaxed))) && snap->id != g_listSnapId.load(std::memory_order_relaxed)) {
			return false;
		}
		// Ertragsbremse und seltenere Pruefung Sichtbarer nur beim Weglassen im Hauptbild (Messen prueft alles)
		const bool fast = cullOn && isMain && !probe;
		const auto frameNo = g_frameNo.load(std::memory_order_relaxed);
		if (fast) {
			if (!g_testFrame.load(std::memory_order_relaxed)) {
				return false;
			}
			if (VisibleSkip(&a_geom, frameNo)) {
				if (diag) {
					g_visSkipped.fetch_add(1, std::memory_order_relaxed);
				}
				return false;
			}
		}
		const auto t0 = diag ? Clock::now() : Clock::time_point{};
		// Kamera dieses Frames (aendert sich waehrend des Sammelns nicht)
		const auto         worldCam = RE::Main::WorldRootCamera();
		const RE::NiPoint3 camNow = worldCam ? worldCam->world.translate : snap->cam.pos;
		const bool         reproject = Config::occlReproject.load(std::memory_order_relaxed);
		const Pyramid*     pyr = nullptr;
		std::uint32_t      streakId = snap->id;
		int                r;
		if (reproject) {
			// Tiefenbild in der Kamera dieses Frames; passt die Pyramide noch nicht, diesmal nichts weglassen
			pyr = worldCam ? PyramidForThread(worldCam->world.translate, worldCam->world.rotate) : nullptr;
			if (!pyr) {
				if (diag) {
					g_kickLate.fetch_add(1, std::memory_order_relaxed);
				}
				RequestPyramid(snap->id);
				if (diag) {
					g_notReady.fetch_add(1, std::memory_order_relaxed);
				}
				return false;
			}
			// Neueres Tiefenbild da (z. B. in Innenraeumen ohne Sonnen-Kamera, oder im Stand): neu aufbauen lassen, bis dahin
			// mit dieser Pyramide pruefen. Sonst bliebe die Nummer im Stand gleich und kein Objekt waere je "zweimal verdeckt".
			if (pyr->snapId != snap->id) {
				RequestPyramid(snap->id);
			}
			r = TestPyramid(*pyr, a_geom.worldBound);
			streakId = pyr->snapId;
		} else {
			// alter Weg: Kachel-Scan im Bild der alten Kamera mit Parallaxe-Ausgleich
			r = Test(*snap, a_geom.worldBound, nullptr, &camNow);
		}
		if (diag) {
			g_testNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count()), std::memory_order_relaxed);
		}
		if (r == 3) {
			if (diag) {
				s.tooBig.fetch_add(1, std::memory_order_relaxed);
			}
			return false;
		}
		if (diag) {
			s.tested.fetch_add(1, std::memory_order_relaxed);
		}
		// Verlauf nur fuer feste Objekte im Hauptbild (Figuren: Huelle hinkt der Animation hinterher)
		const bool twice = isMain && !(skinned && Config::occlSkipSkinned.load(std::memory_order_relaxed)) && UpdateStreak(&a_geom, streakId, r == 2, a_geom.worldBound.center);
		if (r != 2) {
			if (fast) {
				VisibleMark(&a_geom, frameNo);
				CountYield(false);
			}
			return false;
		}
		if (diag) {
			s.occGeoms.fetch_add(1, std::memory_order_relaxed);
			s.occDraws.fetch_add(a_draws, std::memory_order_relaxed);
			const float rad = a_geom.worldBound.radius;
			(rad < 50.0f ? s.occSmall : rad < 200.0f ? s.occMid : s.occLarge).fetch_add(a_draws, std::memory_order_relaxed);
			if (skinned) {
				s.occSkinned.fetch_add(a_draws, std::memory_order_relaxed);
			}
		}
		// Kamera zusaetzlich im SELBEN Frame gegen das benutzte Tiefenbild pruefen: g_cullAllowed entsteht erst am Ende des
		// Tiefenvorpasses, also nach dem Sammeln - bei einem Schnitt (Ich-/Schulterperspektive, Teleport, Moebel) blitzten
		// sonst fuer ein Bild Objekte weg, die vom alten Standpunkt verdeckt waren (Review 1.0.61)
		// Mit Umrechnung gilt das Bild fuer die Kamera dieses Frames - keine Pause beim Drehen oder Laufen noetig
		const bool willCull = cullOn && twice && snap->cam.gpu &&
		                      (reproject ? pyr->valid : g_cullAllowed.load(std::memory_order_relaxed) && CameraStillNear(snap->cam));
		if (fast) {
			CountYield(willCull);
		}
		if (snap->id == g_listSnapId.load(std::memory_order_relaxed)) {
			TestInfo    info;
			Test(*snap, a_geom.worldBound, &info, &camNow);
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
			if (diag) {
				s.culledDraws.fetch_add(a_draws, std::memory_order_relaxed);
				std::scoped_lock l(g_sampleLock);
				if (g_samples.size() < 12) {
					TestInfo info;
					Test(*snap, a_geom.worldBound, &info, &camNow);
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

	void NoteDoorMoving(RE::TESObjectREFR* a_door, float a_seconds) noexcept
	{
		if (!a_door) {
			return;
		}
		g_doorEvents.fetch_add(1, std::memory_order_relaxed);
		try {
			const auto      until = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(a_seconds * 1000.0f));
			const auto      handle = a_door->GetHandle();
			std::lock_guard lock(g_doorLock);
			for (auto& d : g_doors) {
				if (d.handle == handle) {
					d.until = std::max(d.until, until);
					return;
				}
			}
			if (g_doors.size() < 32) {
				g_doors.push_back({ handle, until });
			}
		} catch (...) {
		}
	}

	void Reset() noexcept
	{
		Publish(nullptr);
		g_snapPending = false;
		g_cullAllowed.store(false);
		OcclusionGpu::Reset();
		ClearStreaks();
		g_resetGen.fetch_add(1, std::memory_order_release);
		{
			std::scoped_lock l(g_buildLock);
			g_buildReq.reset();
		}
		g_pyr.store(nullptr);
		g_pyrGen.fetch_add(1, std::memory_order_release);
		g_buildKey.store(0, std::memory_order_relaxed);
		for (auto& v : g_vis) {
			v.store(0, std::memory_order_relaxed);
		}
		DetectOtherCullers();
	}

	void OnViewCamera() noexcept
	{
		if (Active() && Config::occlReproject.load(std::memory_order_relaxed) && Config::occlusionCull.load(std::memory_order_relaxed) &&
			Config::masterEnabled.load(std::memory_order_relaxed) && g_testFrame.load(std::memory_order_relaxed)) {
			RequestPyramid();
		}
	}

	void SetFrameBeginHooked() noexcept { g_frameBeginHooked.store(true, std::memory_order_relaxed); }

	void OnFrameBegin() noexcept
	{
		if (g_frameBeginHooked.load(std::memory_order_relaxed)) {
			AdvanceFrame();
		}
		if (Active() && Config::occlReproject.load(std::memory_order_relaxed) && Config::occlusionCull.load(std::memory_order_relaxed) &&
			Config::masterEnabled.load(std::memory_order_relaxed) && g_testFrame.load(std::memory_order_relaxed)) {
			if (Diag()) {
				g_kickBegin.fetch_add(1, std::memory_order_relaxed);
			}
			RequestPyramid();
		}
	}

	void DetectOtherCullers() noexcept
	{
		bool on = false, installed = false;
		try {
			if (REX::W32::GetModuleHandleW(L"CommunityShaders.dll") && std::filesystem::exists("Data/Shaders/Features/HiZOcclusion.ini")) {
				installed = true;
				bool disabled = false, culling = true;
				for (const char* path : { "Data/SKSE/Plugins/CommunityShaders/SettingsDefault.json", "Data/SKSE/Plugins/CommunityShaders/SettingsUser.json" }) {
					std::ifstream f(path);
					if (!f) {
						continue;
					}
					std::stringstream ss;
					ss << f.rdbuf();
					const auto text = ss.str();
					if (const auto v = JsonBool(text, "HiZOcclusion")) {
						disabled = *v;  // "Disable at Boot"
					}
					if (const auto v = JsonBool(text, "enableHiZCulling")) {
						culling = *v;
					}
				}
				on = !disabled && culling && REL::Module::IsAE() && REL::Module::get().version() < REL::Version(1, 7, 99, 0);
			}
		} catch (...) {
			on = false;
		}
		static bool logged = false;
		const bool  installChanged = installed != g_bottledInstalled.exchange(installed);
		if (on != g_bottledHiZ.exchange(on) || installChanged || !logged) {
			logged = true;
			if (installed) {
				logger::warn("[LightShadowCache] Bottled Shaders is installed - shadow cache for static lights {}", Config::lightShadowCache.allowWithBottled ?
					"stays on (bAllowWithBottledShaders)" : "is off (interior shadows flickered with it; allow in the menu or with bAllowWithBottledShaders)");
			}
			if (on) {
				logger::warn("[Occlusion] Bottled Shaders HiZ Occlusion Culling is active - SPS {}", Config::occlAllowWithBottled.load() ?
					"hides occluded objects anyway (bAllowWithBottledHiZ), both test every object" : "Hide occluded objects stays off (allow in the menu or with bAllowWithBottledHiZ)");
			}
			// Faster Objects (SkyrimPrecombines.dll) pausiert sein Hi-Z selbst, wenn unser Weglassen an ist - nur vermerken
			if (REX::W32::GetModuleHandleW(L"SkyrimPrecombines.dll")) {
				logger::info("[Occlusion] Faster Objects (SkyrimPrecombines.dll) is loaded - its Hi-Z culling pauses while SPS Hide occluded objects is on, its precombines stay");
			}
		}
	}

	bool BottledHiZActive() noexcept { return g_bottledHiZ.load(std::memory_order_relaxed); }
	bool BottledInstalled() noexcept { return g_bottledInstalled.load(std::memory_order_relaxed); }
	bool BottledBlocksLightCache() noexcept { return g_bottledInstalled.load(std::memory_order_relaxed) && !Config::lightShadowCache.allowWithBottled; }
}
