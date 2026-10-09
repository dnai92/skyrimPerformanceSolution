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
		};

		float         g_rightSign = 1.0f, g_upSign = 1.0f;
		bool          g_learned = false;
		float         g_learnA = 0.0f, g_learnB = 0.0f;
		bool          g_learnRev = false;
		std::uint64_t g_learnFrames = 0;
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
		};
		std::atomic<std::shared_ptr<const Snapshot>> g_current;

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
		};
		AccStats                   g_acc[4];
		std::atomic<std::uint64_t> g_accOther{ 0 }, g_noSnapshot{ 0 }, g_testNs{ 0 };
		// Main-Thread-Zaehler
		std::uint64_t g_prepassCalls = 0, g_camFail = 0, g_captures = 0, g_ringFull = 0, g_noSrv = 0, g_reads = 0, g_latencySum = 0;
		float         g_camFailErrSum = 0.0f;
		const char*   g_camLayout = "unknown";

		void Mul(const CamSnap& a_c, const RE::NiPoint3& a_world, float a_out[4]) noexcept
		{
			const float v[4]{ a_c.relative ? a_world.x - a_c.posAdjust.x : a_world.x, a_c.relative ? a_world.y - a_c.posAdjust.y : a_world.y,
				a_c.relative ? a_world.z - a_c.posAdjust.z : a_world.z, 1.0f };
			for (int i = 0; i < 4; ++i) {
				a_out[i] = a_c.colVec ? a_c.m[i][0] * v[0] + a_c.m[i][1] * v[1] + a_c.m[i][2] * v[2] + a_c.m[i][3] * v[3] :
				                        v[0] * a_c.m[0][i] + v[1] * a_c.m[1][i] + v[2] * a_c.m[2][i] + v[3] * a_c.m[3][i];
			}
		}

		// 1 = sichtbar, 2 = verdeckt, 3 = zu gross (nicht geprueft)
		int Test(const Snapshot& a_s, const RE::NiBound& a_b) noexcept
		{
			const auto& c = a_s.cam;
			float       x, y, z;
			Project(c, a_b.center, x, y, z);
			const float rad = std::max(a_b.radius, 0.0f);
			const float wn = z - rad;
			if (!(wn > 16.0f)) {
				return 1;  // reicht bis an die Kamera oder dahinter
			}
			const float dn = c.depthA + c.depthB / wn;  // naechste Stelle der Kugel
			// Ausdehnung konservativ mit der naechsten Tiefe; x/y aus dem Mittelpunkt (perspektivisch leicht zu klein,
			// deshalb unten eine Kachel Rand)
			const float ex = rad / wn * 2.0f / (c.r - c.l), ey = rad / wn * 2.0f / (c.t - c.b);
			constexpr float W = static_cast<float>(OcclusionGpu::kWidth), H = static_cast<float>(OcclusionGpu::kHeight);
			// eine Kachel Rand: Bild ist 1-2 Frames alt
			const int u0 = std::max(0, static_cast<int>(std::floor(((x - ex) * 0.5f + 0.5f) * W)) - 1);
			const int u1 = std::min(static_cast<int>(W) - 1, static_cast<int>(std::floor(((x + ex) * 0.5f + 0.5f) * W)) + 1);
			const int v0 = std::max(0, static_cast<int>(std::floor((0.5f - (y + ey) * 0.5f) * H)) - 1);
			const int v1 = std::min(static_cast<int>(H) - 1, static_cast<int>(std::floor((0.5f - (y - ey) * 0.5f) * H)) + 1);
			if (u0 > u1 || v0 > v1) {
				return 1;  // ausserhalb des alten Bildausschnitts - nichts bekannt
			}
			if ((u1 - u0 + 1) * (v1 - v0 + 1) > 4096) {
				return 3;
			}
			const auto& lo = a_s.rb.minDepth;
			const auto& hi = a_s.rb.maxDepth;
			for (int v = v0; v <= v1; ++v) {
				const auto row = static_cast<std::size_t>(v) * OcclusionGpu::kWidth;
				for (int u = u0; u <= u1; ++u) {
					if (c.reversed ? dn >= lo[row + u] : dn <= hi[row + u]) {
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

		void Report()
		{
			const auto now = Clock::now();
			if (now - g_reportStart < std::chrono::seconds(60)) {
				return;
			}
			const double secs = std::chrono::duration<double>(now - g_reportStart).count();
			g_reportStart = now;
			const double frames = std::max<double>(1.0, static_cast<double>(g_frame));
			const auto   cur = g_current.load();
			std::uint32_t sky = 0;
			if (cur) {
				for (std::size_t i = 0; i < cur->rb.maxDepth.size(); ++i) {
					sky += cur->cam.reversed ? cur->rb.minDepth[i] <= 1e-6f : cur->rb.maxDepth[i] >= 0.99999f;
				}
			}
			logger::info("[Occlusion] {:.0f} s, {:.0f} frames | prepass {} | renderer matrix {} usable {} frames, not {} (avg w error {:.0f}) | own projection: right {:+.0f} up {:+.0f}, depth {} A {:.5f} B {:.3f}{}, max NDC diff {:.3f} | depth source {} ({}x{}) | captures {}, ring full {}, no depth {} | reads {}, latency {:.1f} frames | sky tiles {:.0f} % | test time {:.2f} ms/frame (all threads) | no snapshot yet {}",
				secs, frames, g_prepassCalls, g_camLayout, g_learnFrames, g_camFail, g_camFail ? g_camFailErrSum / g_camFail : 0.0f, g_rightSign, g_upSign,
				g_learned ? "learned" : "formula", g_learnA, g_learnB, g_learnRev ? " reversed" : "", g_learnMaxErr, g_lastSource == 0 ? "post-prepass copy" : "main",
				g_lastSrcW, g_lastSrcH, g_captures, g_ringFull, g_noSrv, g_reads, g_reads ? static_cast<double>(g_latencySum) / g_reads : 0.0,
				cur ? 100.0 * sky / cur->rb.maxDepth.size() : 0.0, g_testNs.exchange(0) / 1e6 / frames, g_noSnapshot.exchange(0));
			for (auto& s : g_acc) {
				const auto geoms = s.geoms.exchange(0);
				const auto acc = s.acc.exchange(nullptr);
				const auto draws = s.draws.exchange(0), tested = s.tested.exchange(0), occGeoms = s.occGeoms.exchange(0), occDraws = s.occDraws.exchange(0);
				const auto tooBig = s.tooBig.exchange(0), occSkinned = s.occSkinned.exchange(0), occSmall = s.occSmall.exchange(0), occMid = s.occMid.exchange(0), occLarge = s.occLarge.exchange(0);
				if (!acc || !geoms) {
					continue;
				}
				logger::info("[Occlusion]   accumulator {} | per frame: objects {:.0f}, draws {:.0f}, tested {:.0f} | occluded: objects {:.0f} ({:.0f} % of tested), draws {:.0f} ({:.0f} % of all) | occluded draws by radius <50 {:.0f}, <200 {:.0f}, larger {:.0f}, skinned {:.0f} | too big {:.0f}",
					acc, geoms / frames, draws / frames, tested / frames, occGeoms / frames, tested ? 100.0 * occGeoms / tested : 0.0, occDraws / frames,
					draws ? 100.0 * occDraws / draws : 0.0, occSmall / frames, occMid / frames, occLarge / frames, occSkinned / frames, tooBig / frames);
			}
			g_accOther = 0;
			g_prepassCalls = g_camFail = g_captures = g_ringFull = g_noSrv = g_reads = g_latencySum = 0;
			g_camFailErrSum = 0.0f;
			g_learnFrames = 0;
			g_learnMaxErr = 0.0f;
			g_frame = 0;
		}
	}

	void OnDepthPrepassEnd() noexcept
	{
		if (!Config::occlusionProbe.load(std::memory_order_relaxed)) {
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
		g_snapPending = true;

		// Abgleich mit der Renderer-Matrix, wenn sie in diesem Frame zur Hauptkamera gehoert
		auto&      rd = state->GetRuntimeData();
		const auto view = rd.cameraData.getEye();
		CamSnap    s{};
		std::memcpy(s.m, &view.viewProjMatrixUnjittered, sizeof(s.m));
		s.posAdjust = rd.posAdjust.getEye();
		// Konvention bestimmen: ein Punkt 1000 Einheiten vor der Kamera muss w ~ 1000 ergeben
		const RE::NiPoint3 fwds[2]{ own.dir, { view.viewForward.x, view.viewForward.y, view.viewForward.z } };
		RE::NiPoint3       fwd = fwds[0];
		const auto         at = [&](float a_d) { return RE::NiPoint3{ camPos.x + fwd.x * a_d, camPos.y + fwd.y * a_d, camPos.z + fwd.z * a_d }; };
		float              best = 1e9f;
		RE::NiPoint3       bestFwd = fwds[0];
		for (const auto& f : fwds) {
			fwd = f;
			for (int k = 0; k < 4; ++k) {
				CamSnap t = s;
				t.colVec = (k & 1) == 0;
				t.relative = (k & 2) == 0;
				float c[4];
				Mul(t, at(1000.0f), c);
				const float err = std::abs(c[3] - 1000.0f);
				if (err < best) {
					best = err;
					s.colVec = t.colVec;
					s.relative = t.relative;
					bestFwd = f;
				}
			}
		}
		fwd = bestFwd;
		if (best > 50.0f) {
			++g_camFail;  // Renderer-Matrix gehoert gerade nicht zur Hauptkamera - eigene Projektion gilt trotzdem
			g_camFailErrSum += std::min(best, 1e6f);
			return;
		}
		g_camLayout = s.colVec ? (s.relative ? "col/rel" : "col/abs") : (s.relative ? "row/rel" : "row/abs");
		// Tiefenformel aus der Renderer-Matrix
		float c1[4], c2[4];
		Mul(s, at(100.0f), c1);
		Mul(s, at(10000.0f), c2);
		const float d1 = c1[2] / c1[3], d2 = c2[2] / c2[3];
		g_learnRev = d1 > d2;
		g_learnB = (d1 - d2) / (1.0f / c1[3] - 1.0f / c2[3]);
		g_learnA = d1 - g_learnB / c1[3];
		// Achsen-Vorzeichen: Testpunkt rechts oben vor der Kamera, Renderer- gegen eigene Projektion
		const RE::NiPoint3 rawUp{ rot.entry[0][1], rot.entry[1][1], rot.entry[2][1] };
		const RE::NiPoint3 rawRight{ rot.entry[0][2], rot.entry[1][2], rot.entry[2][2] };
		const RE::NiPoint3 p{ camPos.x + own.dir.x * 1000.0f + rawRight.x * 200.0f + rawUp.x * 100.0f, camPos.y + own.dir.y * 1000.0f + rawRight.y * 200.0f + rawUp.y * 100.0f,
			camPos.z + own.dir.z * 1000.0f + rawRight.z * 200.0f + rawUp.z * 100.0f };
		float rc[4];
		Mul(s, p, rc);
		const float rx = rc[0] / rc[3], ry = rc[1] / rc[3];
		OwnCam probe = own;
		probe.right = rawRight;
		probe.up = rawUp;
		float ox, oy, oz;
		Project(probe, p, ox, oy, oz);
		g_rightSign = (rx > 0.0f) == (ox > 0.0f) ? 1.0f : -1.0f;
		g_upSign = (ry > 0.0f) == (oy > 0.0f) ? 1.0f : -1.0f;
		g_learnMaxErr = std::max({ g_learnMaxErr, std::abs(rx - ox * g_rightSign), std::abs(ry - oy * g_upSign) });
		g_learned = true;
		++g_learnFrames;
	}

	void OnPresent() noexcept
	{
		++g_frame;
		if (!Config::occlusionProbe.load(std::memory_order_relaxed)) {
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
			logger::info("[Occlusion] probe ready ({}x{} tiles, measuring only)", OcclusionGpu::kWidth, OcclusionGpu::kHeight);
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
			g_current.store(std::move(newest));
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

	void CountMain(const RE::BSGeometry& a_geom, const void* a_accumulator, std::uint32_t a_draws) noexcept
	{
		if (!Config::occlusionProbe.load(std::memory_order_relaxed)) {
			return;
		}
		auto& s = Slot(a_accumulator);
		s.geoms.fetch_add(1, std::memory_order_relaxed);
		s.draws.fetch_add(a_draws, std::memory_order_relaxed);
		const auto snap = g_current.load();
		if (!snap) {
			g_noSnapshot.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		const auto t0 = Clock::now();
		const int  r = Test(*snap, a_geom.worldBound);
		g_testNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count()), std::memory_order_relaxed);
		if (r == 3) {
			s.tooBig.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		s.tested.fetch_add(1, std::memory_order_relaxed);
		if (r != 2) {
			return;
		}
		s.occGeoms.fetch_add(1, std::memory_order_relaxed);
		s.occDraws.fetch_add(a_draws, std::memory_order_relaxed);
		const float rad = a_geom.worldBound.radius;
		(rad < 50.0f ? s.occSmall : rad < 200.0f ? s.occMid : s.occLarge).fetch_add(a_draws, std::memory_order_relaxed);
		if (const_cast<RE::BSGeometry&>(a_geom).GetGeometryRuntimeData().skinInstance) {
			s.occSkinned.fetch_add(a_draws, std::memory_order_relaxed);
		}
	}

	void Reset() noexcept
	{
		g_current.store(nullptr);
		g_snapPending = false;
		OcclusionGpu::Reset();
	}
}
