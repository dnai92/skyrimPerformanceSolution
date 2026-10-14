#include "PrepassAnalysis.h"

#include "GpuTimer.h"

namespace PrepassAnalysis
{
	namespace
	{
		enum Kind : int
		{
			kOther,
			kGrass,
			kAlpha,      // Alpha-Test (Laub, Zaeune, Stoff) ohne Gras/Baeume
			kTree,       // Baeume mit Wind (kTreeAnim) und ferne LOD-Baeume
			kLandscape,  // Gelaende und LOD-Gelaende
			kSmall,      // klein und fern (Radius/Entfernung < 0,025 ab 1500 Einheiten)
			kActor,      // geskinnt (Figuren, Ruestung)
			kKindCount
		};
		constexpr const char* kKindNames[kKindCount]{ "other", "grass", "alpha-tested", "trees", "landscape", "small far", "skinned" };

		// Phase -> weggelassene Art (kOther = nichts weglassen). Zwei Runden.
		constexpr Kind kModes[]{ kOther, kGrass, kAlpha, kTree, kLandscape, kSmall, kActor };
		constexpr int  kModeCount = static_cast<int>(std::size(kModes));
		constexpr int  kRounds = 2;
		constexpr auto kPhaseLength = std::chrono::seconds(8);
		constexpr auto kSettle = std::chrono::milliseconds(500);  // GPU-Ergebnisse kommen einige Frames verspaetet

		struct Phase
		{
			std::uint64_t frames = 0;
			double        cpuMs = 0, gpuMs = 0, frameMs = 0;
			std::uint32_t gpuFrames = 0;
			double        draws[kKindCount]{};
			double        skipped = 0;
		};

		std::atomic<bool>                     g_start{ false }, g_running{ false };
		int                                   g_phase = -1;
		Phase                                 g_cur{};
		std::array<Phase, kModeCount>         g_total{};
		std::chrono::steady_clock::time_point g_phaseStart{}, g_lastFrame{};
		bool                                  g_settled = false;
		std::int64_t                          g_cpuNs = 0;
		std::uint32_t                         g_frameDraws[kKindCount]{};
		std::uint32_t                         g_frameSkipped = 0;
		RE::NiPoint3                          g_cam{};

		Kind Classify(const RE::BSRenderPass& a_pass, bool a_alphaTest) noexcept
		{
			const auto geom = a_pass.geometry;
			if (a_pass.shader && a_pass.shader->shaderType.get() == RE::BSShader::Type::Grass) {
				return kGrass;
			}
			if (a_pass.shader && a_pass.shader->shaderType.get() == RE::BSShader::Type::DistantTree) {
				return kTree;
			}
			if (geom && geom->GetGeometryRuntimeData().skinInstance) {
				return kActor;
			}
			if (const auto prop = a_pass.shaderProperty) {
				using F = RE::BSShaderProperty::EShaderPropertyFlag;
				if (prop->flags.any(F::kTreeAnim)) {
					return kTree;
				}
				if (prop->flags.any(F::kMultiTextureLandscape, F::kLODLandscape)) {
					return kLandscape;
				}
			}
			if (geom) {
				const auto& b = geom->worldBound;
				const float dx = b.center.x - g_cam.x, dy = b.center.y - g_cam.y, dz = b.center.z - g_cam.z;
				const float d = std::sqrt(dx * dx + dy * dy + dz * dz) - b.radius;
				if (d > 1500.0f && b.radius < 0.025f * d) {
					return kSmall;
				}
			}
			return a_alphaTest ? kAlpha : kOther;
		}

		void BeginPhase(int a_phase, std::chrono::steady_clock::time_point a_now) noexcept
		{
			g_phase = a_phase;
			g_cur = {};
			g_phaseStart = a_now;
			g_settled = false;
		}

		void EndPhase() noexcept
		{
			const auto  mode = g_phase % kModeCount;
			const auto& p = g_cur;
			const double f = p.frames ? static_cast<double>(p.frames) : 1.0;
			const double g = p.gpuFrames ? static_cast<double>(p.gpuFrames) : 1.0;
			logger::info("[Prepass-AB] phase {} without {}: {} frames | prepass GPU {:.2f} ms, CPU {:.2f} ms | frame {:.2f} ms | draws skipped {:.0f}/frame",
				g_phase + 1, kModes[mode] == kOther ? "nothing" : kKindNames[kModes[mode]], p.frames, p.gpuMs / g, p.cpuMs / f, p.frameMs / f, p.skipped / f);
			auto& t = g_total[mode];
			t.frames += p.frames;
			t.cpuMs += p.cpuMs;
			t.frameMs += p.frameMs;
			t.gpuMs += p.gpuMs;
			t.gpuFrames += p.gpuFrames;
			t.skipped += p.skipped;
			for (int k = 0; k < kKindCount; ++k) {
				t.draws[k] += p.draws[k];
			}
		}

		void Finish() noexcept
		{
			const auto& base = g_total[0];
			const double bg = base.gpuFrames ? base.gpuMs / base.gpuFrames : 0.0;
			const double bc = base.frames ? base.cpuMs / static_cast<double>(base.frames) : 0.0;
			std::string  draws;
			for (int k = 0; k < kKindCount; ++k) {
				draws += std::format("{}{} {:.0f}", k ? ", " : "", kKindNames[k], base.frames ? base.draws[k] / static_cast<double>(base.frames) : 0.0);
			}
			logger::info("[Prepass-AB] result: full prepass GPU {:.2f} ms, CPU {:.2f} ms | draws/frame: {}", bg, bc, draws);
			for (int m = 1; m < kModeCount; ++m) {
				const auto&  t = g_total[m];
				const double tg = t.gpuFrames ? t.gpuMs / t.gpuFrames : 0.0;
				const double tc = t.frames ? t.cpuMs / static_cast<double>(t.frames) : 0.0;
				logger::info("[Prepass-AB]   without {:<12}: GPU {:.2f} ms ({:+.2f}) | CPU {:.2f} ms ({:+.2f}) | {:.0f} draws left out", kKindNames[kModes[m]], tg, tg - bg, tc, tc - bc,
					t.frames ? t.skipped / static_cast<double>(t.frames) : 0.0);
			}
			g_running.store(false);
			g_phase = -1;
		}
	}

	void Start() noexcept
	{
		if (!g_running.load()) {
			g_start.store(true);
		}
	}

	bool Running() noexcept { return g_running.load(std::memory_order_relaxed) || g_start.load(std::memory_order_relaxed); }

	void OnFrame() noexcept
	{
		const auto now = std::chrono::steady_clock::now();
		if (const auto cam = RE::Main::WorldRootCamera()) {
			g_cam = cam->world.translate;
		}
		const std::int64_t cpu = g_cpuNs;
		std::uint32_t      draws[kKindCount];
		std::copy(std::begin(g_frameDraws), std::end(g_frameDraws), draws);
		const std::uint32_t skipped = g_frameSkipped;
		g_cpuNs = 0;
		std::fill(std::begin(g_frameDraws), std::end(g_frameDraws), 0u);
		g_frameSkipped = 0;

		if (g_start.exchange(false)) {
			g_total = {};
			g_running.store(true);
			logger::info("[Prepass-AB] start: {} phases of {} s, leaving out one kind of object in the depth pre-pass at a time (stand still; the image can look wrong meanwhile)",
				kModeCount * kRounds, kPhaseLength.count());
			BeginPhase(0, now);
			g_lastFrame = now;
			GpuTimer::TakeTap(GpuTimer::kDepthPrepass);
			return;
		}
		if (!g_running.load(std::memory_order_relaxed)) {
			return;
		}
		const double dt = std::chrono::duration<double, std::milli>(now - g_lastFrame).count();
		g_lastFrame = now;
		// GPU-Ergebnisse dieses Frames stammen von einigen Frames zuvor - nach dem Phasenwechsel erst verwerfen
		const auto tap = GpuTimer::TakeTap(GpuTimer::kDepthPrepass);
		if (!g_settled) {
			g_settled = now - g_phaseStart >= kSettle;
		} else if (dt > 0.0 && dt < 250.0) {
			auto& p = g_cur;
			++p.frames;
			p.cpuMs += static_cast<double>(cpu) / 1e6;
			p.frameMs += dt;
			p.gpuMs += tap.ms;
			p.gpuFrames += tap.frames;
			p.skipped += skipped;
			for (int k = 0; k < kKindCount; ++k) {
				p.draws[k] += draws[k];
			}
		}
		if (now - g_phaseStart >= kPhaseLength) {
			EndPhase();
			if (g_phase + 1 < kModeCount * kRounds) {
				BeginPhase(g_phase + 1, now);
			} else {
				Finish();
			}
		}
	}

	bool SkipDraw(const RE::BSRenderPass& a_pass, bool a_alphaTest) noexcept
	{
		if (!g_running.load(std::memory_order_relaxed) || g_phase < 0) {
			return false;
		}
		const auto kind = Classify(a_pass, a_alphaTest);
		++g_frameDraws[kind];
		const auto off = kModes[g_phase % kModeCount];
		if (off != kOther && kind == off) {
			++g_frameSkipped;
			return true;
		}
		return false;
	}

	void AddCpuNs(std::int64_t a_ns) noexcept
	{
		if (g_running.load(std::memory_order_relaxed)) {
			g_cpuNs += a_ns;
		}
	}
}
