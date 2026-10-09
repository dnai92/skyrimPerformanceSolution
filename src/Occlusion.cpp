#include "Occlusion.h"

#include "Config.h"
#include "OcclusionGpu.h"

namespace Occlusion
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// Kamera eines Frames, wie sie beim Tiefenvorpass galt. Die Projektion wird beim Merken selbst geprueft:
		// Matrix-Konvention (Zeilen-/Spaltenvektor) und ob Positionen relativ zu posAdjust erwartet werden.
		struct CamSnap
		{
			float        m[4][4]{};
			bool         colVec = true;     // clip = M * v (sonst v * M)
			bool         relative = true;   // v = Welt - posAdjust
			RE::NiPoint3 posAdjust{};
			float        depthA = 0.0f, depthB = 0.0f;  // Tiefe(NDC) = A + B / w
			bool         reversed = false;              // fern = kleiner Wert
			float        sx = 1.0f, sy = 1.0f;          // Projektions-Skalierung x/y
			std::uint64_t frame = 0;
			bool         valid = false;
		};

		constexpr std::uint32_t kSnaps = 8;
		CamSnap       g_snaps[kSnaps];
		std::uint32_t g_nextTag = 0;
		bool          g_snapPending = false;  // Kamera dieses Frames gemerkt, Tiefe noch nicht eingereicht
		std::uint64_t g_frame = 0;

		OcclusionGpu::Readback g_cur;
		CamSnap                g_curCam;
		bool                   g_curValid = false;
		std::uint64_t          g_curFrame = 0;  // Frame, aus dem das aktuelle Bild stammt

		// Minutenbericht
		struct AccStats
		{
			const void*   acc = nullptr;
			std::uint64_t geoms = 0, draws = 0, tested = 0, occGeoms = 0, occDraws = 0, tooBig = 0;
			std::uint64_t occSkinned = 0, occSmall = 0, occMid = 0, occLarge = 0;  // verdeckte Draws nach Radius <50 / <200 / groesser
		};
		AccStats      g_acc[4];
		std::uint64_t g_accOther = 0;
		std::uint64_t g_captures = 0, g_ringFull = 0, g_noSrv = 0, g_reads = 0, g_latencySum = 0;
		std::uint64_t g_testNs = 0;
		std::uint32_t g_lastSource = 0;
		Clock::time_point g_reportStart = Clock::now();
		bool          g_initTried = false;
		bool          g_snapFailLogged = false;
		std::uint32_t g_mainThread = 0;      // Thread des Present (= Render-/Main-Thread)
		std::uint64_t g_otherThread = 0;     // Aufrufe von anderen Threads (nicht gezaehlt)

		void Mul(const CamSnap& a_c, const RE::NiPoint3& a_world, float a_out[4]) noexcept
		{
			const float v[4]{ a_c.relative ? a_world.x - a_c.posAdjust.x : a_world.x, a_c.relative ? a_world.y - a_c.posAdjust.y : a_world.y,
				a_c.relative ? a_world.z - a_c.posAdjust.z : a_world.z, 1.0f };
			for (int i = 0; i < 4; ++i) {
				a_out[i] = a_c.colVec ? a_c.m[i][0] * v[0] + a_c.m[i][1] * v[1] + a_c.m[i][2] * v[2] + a_c.m[i][3] * v[3] :
				                        v[0] * a_c.m[0][i] + v[1] * a_c.m[1][i] + v[2] * a_c.m[2][i] + v[3] * a_c.m[3][i];
			}
		}

		// 0 = nicht pruefbar, 1 = sichtbar, 2 = verdeckt, 3 = zu gross (nicht geprueft)
		int Test(const RE::NiBound& a_b) noexcept
		{
			const auto& c = g_curCam;
			float       clip[4];
			Mul(c, a_b.center, clip);
			const float w = clip[3], r = std::max(a_b.radius, 0.0f);
			const float wn = w - r;
			if (!(wn > 16.0f)) {
				return 1;  // reicht bis an die Kamera oder dahinter
			}
			const float dn = c.depthA + c.depthB / wn;  // naechste Stelle der Kugel
			const float x = clip[0] / w, y = clip[1] / w;
			const float ex = r * c.sx / wn, ey = r * c.sy / wn;
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
			for (int v = v0; v <= v1; ++v) {
				const auto row = static_cast<std::size_t>(v) * OcclusionGpu::kWidth;
				for (int u = u0; u <= u1; ++u) {
					if (c.reversed ? dn >= g_cur.minDepth[row + u] : dn <= g_cur.maxDepth[row + u]) {
						return 1;
					}
				}
			}
			return 2;
		}

		AccStats& Slot(const void* a_acc) noexcept
		{
			for (auto& s : g_acc) {
				if (s.acc == a_acc) {
					return s;
				}
			}
			for (auto& s : g_acc) {
				if (!s.acc) {
					s.acc = a_acc;
					return s;
				}
			}
			++g_accOther;
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
			std::uint32_t sky = 0;
			if (g_curValid) {
				for (std::size_t i = 0; i < g_cur.maxDepth.size(); ++i) {
					sky += g_curCam.reversed ? g_cur.minDepth[i] <= 1e-6f : g_cur.maxDepth[i] >= 0.99999f;
				}
			}
			logger::info("[Occlusion] {:.0f} s | depth source {} ({}x{}) | captures {}, ring full {}, no depth {} | reads {}, latency {:.1f} frames | camera {}{}{} | sky tiles {:.0f} % | test time {:.2f} ms/frame | calls from other threads {}",
				secs, g_lastSource == 0 ? "post-prepass copy" : "main", g_cur.srcW, g_cur.srcH, g_captures, g_ringFull, g_noSrv, g_reads,
				g_reads ? static_cast<double>(g_latencySum) / g_reads : 0.0, g_curCam.valid ? (g_curCam.colVec ? "col" : "row") : "invalid",
				g_curCam.relative ? "/rel" : "/abs", g_curCam.reversed ? "/reversed-z" : "/standard-z",
				g_curValid ? 100.0 * sky / g_cur.maxDepth.size() : 0.0, g_testNs / 1e6 / frames, g_otherThread);
			for (const auto& s : g_acc) {
				if (!s.acc || !s.geoms) {
					continue;
				}
				logger::info("[Occlusion]   accumulator {} | per frame: objects {:.0f}, draws {:.0f}, tested {:.0f} | occluded: objects {:.0f} ({:.0f} %), draws {:.0f} ({:.0f} %) | occluded draws by radius <50 {:.0f}, <200 {:.0f}, larger {:.0f}, skinned {:.0f} | too big {:.0f}",
					s.acc, s.geoms / frames, s.draws / frames, s.tested / frames, s.occGeoms / frames, s.tested ? 100.0 * s.occGeoms / s.tested : 0.0,
					s.occDraws / frames, s.draws ? 100.0 * s.occDraws / s.draws : 0.0, s.occSmall / frames, s.occMid / frames, s.occLarge / frames,
					s.occSkinned / frames, s.tooBig / frames);
			}
			for (auto& s : g_acc) {
				s = {};
			}
			g_otherThread = g_accOther = g_captures = g_ringFull = g_noSrv = g_reads = g_latencySum = g_testNs = 0;
			g_frame = 0;
		}
	}

	void OnDepthPrepassEnd() noexcept
	{
		if (!Config::occlusionProbe.load(std::memory_order_relaxed)) {
			return;
		}
		const auto state = RE::BSGraphics::RendererShadowState::GetSingleton();
		const auto cam = RE::Main::WorldRootCamera();
		if (!state || !cam) {
			return;
		}
		auto&       rd = state->GetRuntimeData();
		const auto  view = rd.cameraData.getEye();
		auto&       s = g_snaps[g_nextTag % kSnaps];
		s = {};
		std::memcpy(s.m, &view.viewProjMatrixUnjittered, sizeof(s.m));
		s.posAdjust = rd.posAdjust.getEye();
		s.sx = std::abs(view.projMatrixUnjittered.m[0][0]);
		s.sy = std::abs(view.projMatrixUnjittered.m[1][1]);
		// Konvention bestimmen: ein Punkt 1000 Einheiten vor der Kamera muss w ~ 1000 ergeben
		// Blickrichtung: Spalte 0 der Kamera (Gamebryo) oder viewForward der Renderer-Kamera - die passende gewinnt
		const auto&        camPos = cam->world.translate;
		const auto&        rot = cam->world.rotate;
		const RE::NiPoint3 fwds[2]{ { rot.entry[0][0], rot.entry[1][0], rot.entry[2][0] }, { view.viewForward.x, view.viewForward.y, view.viewForward.z } };
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
			if (!g_snapFailLogged) {
				g_snapFailLogged = true;
				logger::warn("[Occlusion] camera matrix not understood (w error {:.1f} at 1000 units) - probe inactive", best);
			}
			return;
		}
		float c1[4], c2[4];
		Mul(s, at(100.0f), c1);
		Mul(s, at(10000.0f), c2);
		const float d1 = c1[2] / c1[3], d2 = c2[2] / c2[3];
		s.reversed = d1 > d2;
		s.depthB = (d1 - d2) / (1.0f / c1[3] - 1.0f / c2[3]);
		s.depthA = d1 - s.depthB / c1[3];
		s.frame = g_frame;
		s.valid = true;
		g_snapPending = true;
	}

	void OnPresent() noexcept
	{
		++g_frame;
		if (!Config::occlusionProbe.load(std::memory_order_relaxed)) {
			g_curValid = false;
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
		// fertige Ergebnisse abholen (das neueste gilt)
		OcclusionGpu::Readback rb;
		while (OcclusionGpu::Poll(ctx, rb)) {
			const auto& snap = g_snaps[rb.tag % kSnaps];
			if (!snap.valid) {
				continue;
			}
			g_cur = std::move(rb);
			g_curCam = snap;
			g_curFrame = snap.frame;
			g_curValid = true;
			++g_reads;
			g_latencySum += g_frame - snap.frame;
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
		// nur im Main-Thread: dort wird auch das Tiefenbild ausgetauscht
		if (REX::W32::GetCurrentThreadId() != g_mainThread) {
			++g_otherThread;
			return;
		}
		auto& s = Slot(a_accumulator);
		++s.geoms;
		s.draws += a_draws;
		if (!g_curValid) {
			return;
		}
		const auto t0 = Clock::now();
		const int  r = Test(a_geom.worldBound);
		g_testNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
		if (r == 3) {
			++s.tooBig;
			return;
		}
		++s.tested;
		if (r != 2) {
			return;
		}
		++s.occGeoms;
		s.occDraws += a_draws;
		const float rad = a_geom.worldBound.radius;
		(rad < 50.0f ? s.occSmall : rad < 200.0f ? s.occMid : s.occLarge) += a_draws;
		if (const_cast<RE::BSGeometry&>(a_geom).GetGeometryRuntimeData().skinInstance) {
			s.occSkinned += a_draws;
		}
	}

	void Reset() noexcept
	{
		g_curValid = false;
		g_snapPending = false;
		OcclusionGpu::Reset();
	}
}
