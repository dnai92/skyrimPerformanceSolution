#include "EngineTimers.h"
#include "Features.h"

#include "Config.h"
#include "DetourHelper.h"
#include "InstancingAnalysis.h"
#include "Stats.h"

namespace EngineTimers
{
	namespace
	{
		// SetupMask wird mit Community Shaders nicht ueber die Aufrufstelle in ID 26175 aufgerufen (Zeit dort 0),
		// daher am Funktionsanfang per Detours (verkettet sich mit evtl. vorhandenen Detours anderer Mods)
		// A/B-Messung Himmelslicht-Culling
		std::atomic<bool>          g_abRunning{ false };
		std::atomic<std::uint64_t> g_abMaskNs{ 0 }, g_abMaskCalls{ 0 }, g_abCulled{ 0 }, g_abKept{ 0 };
		bool                       g_precipInstalled = false;

		struct PrecipSetupMask
		{
			static void thunk(void* a_precipitation)
			{
				Stats::ScopedTimer timer(Stats::Zone::PrecipMask);
				if (!g_abRunning.load(std::memory_order_relaxed)) {
					func(a_precipitation);
					return;
				}
				const auto t0 = std::chrono::steady_clock::now();
				func(a_precipitation);
				g_abMaskNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()),
					std::memory_order_relaxed);
				g_abMaskCalls.fetch_add(1, std::memory_order_relaxed);
			}
			static inline void (*func)(void*) = nullptr;
		};

		// 3 Varianten im Wechsel, je 3 Durchgaenge: 0 = aus, 1 = nur Einzel-Objekte, 2 = Einzel-Objekte + Teilbaeume
		constexpr int  kAbModes = 3;
		constexpr int  kAbPhases = 9;
		constexpr auto kAbPhaseLength = std::chrono::seconds(15);
		struct AbPhase
		{
			int           mode = 0;
			std::uint64_t frames = 0, ns = 0, calls = 0, culled = 0, kept = 0;
		};
		std::array<AbPhase, kAbPhases>        g_abPhases;
		int                                   g_abPhase = -1;
		bool                                  g_abOriginal = true, g_abOriginalPrune = true;
		std::chrono::steady_clock::time_point g_abPhaseStart{};
		std::atomic<bool>                     g_abStartRequested{ false };

		void AbBeginPhase(int a_phase)
		{
			g_abPhase = a_phase;
			// Reihum (Reihenfolge-Effekte wie Nachladen mitteln sich ueber die Durchgaenge heraus)
			const int mode = a_phase % kAbModes;
			Config::skylightingCulling.enabled = mode != 0;
			Config::subtreePruning.precip = mode == 2;
			g_abPhases[a_phase] = AbPhase{ mode };
			g_abMaskNs.store(0);
			g_abMaskCalls.store(0);
			g_abCulled.store(0);
			g_abKept.store(0);
			g_abPhaseStart = std::chrono::steady_clock::now();
		}

		void AbEndPhase()
		{
			auto& p = g_abPhases[g_abPhase];
			p.ns = g_abMaskNs.load();
			p.calls = g_abMaskCalls.load();
			p.culled = g_abCulled.load();
			p.kept = g_abKept.load();
			const double f = p.frames ? static_cast<double>(p.frames) : 1.0;
			static constexpr const char* kNames[] = { "off", "objects only", "objects + groups" };
			logger::info("[Skylight-AB] phase {} culling {}: {} frames | rain/sky map {:.3f} ms/frame ({:.2f} calls/frame) | objects drawn {:.0f}/frame, left out {:.0f}/frame",
				g_abPhase + 1, kNames[p.mode], p.frames, p.ns / 1e6 / f, p.calls / f, p.kept / f, p.culled / f);
		}

		void AbFinish()
		{
			std::array<double, kAbModes> ns{}, fr{}, kept{};
			for (const auto& p : g_abPhases) {
				ns[p.mode] += static_cast<double>(p.ns);
				fr[p.mode] += static_cast<double>(p.frames);
				kept[p.mode] += static_cast<double>(p.kept);
			}
			const auto ms = [&](int m) { return fr[m] > 0 ? ns[m] / 1e6 / fr[m] : 0.0; };
			const auto fps = [&](int m) { return fr[m] / (kAbPhases / kAbModes * static_cast<double>(kAbPhaseLength.count())); };
			const bool cs = GetModuleHandleW(L"CommunityShaders.dll") != nullptr;
			logger::info("[Skylight-AB] result: rain/sky map off {:.3f} ms ({:.1f} FPS) | objects only {:.3f} ms ({:.1f} FPS) | objects + groups {:.3f} ms ({:.1f} FPS) | "
						 "objects drawn {:.0f} / {:.0f} / {:.0f} per frame | Community Shaders {}",
				ms(0), fps(0), ms(1), fps(1), ms(2), fps(2), fr[0] > 0 ? kept[0] / fr[0] : 0.0, fr[1] > 0 ? kept[1] / fr[1] : 0.0, fr[2] > 0 ? kept[2] / fr[2] : 0.0,
				cs ? "loaded" : "not loaded");
			Config::skylightingCulling.enabled = g_abOriginal;
			Config::subtreePruning.precip = g_abOriginalPrune;
			g_abRunning.store(false);
			g_abPhase = -1;
		}

		struct MainCull
		{
			static void thunk(void* a_arg1, void* a_arg2)
			{
				Stats::ScopedTimer timer(Stats::Zone::MainCull);
				func(a_arg1, a_arg2);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Aufrufstelle pruefen (call rel32 auf das erwartete Ziel), sonst nicht anfassen
		bool CheckCall(std::uintptr_t a_site, std::uint64_t a_targetId)
		{
			const auto target = REL::Relocation<std::uintptr_t>{ REL::ID(a_targetId) }.address();
			return *reinterpret_cast<std::uint8_t*>(a_site) == 0xE8 && a_site + 5 + *reinterpret_cast<std::int32_t*>(a_site + 1) == target;
		}
	}

	void Install()
	{
		auto& trampoline = SKSE::GetTrampoline();
		PrecipSetupMask::func = reinterpret_cast<void (*)(void*)>(REL::Relocation<std::uintptr_t>{ RELOCATION_ID(25641, 26183) }.address());
		if (const auto err = DetourHelper::Attach(reinterpret_cast<void**>(&PrecipSetupMask::func), reinterpret_cast<void*>(&PrecipSetupMask::thunk)); err == 0) {
			g_precipInstalled = true;
			logger::info("Timing installed: Precipitation::SetupMask (rain/sky map, Detours)");
			Features::Report("Timing: rain/sky map", "Zeitmessung: Regen-/Himmelskarte", true);
		} else {
			Features::Report("Timing: rain/sky map", "Zeitmessung: Regen-/Himmelskarte", false, std::format("Detours error {}", err));
		}
		// Nur AE: Aufrufstelle fuer SE 1.5.97 nicht ermittelt (reine Zeitmessung)
		if (!REL::Module::IsAE()) {
			Features::Report("Timing: main camera culling", "Zeitmessung: Hauptkamera", false, "Anniversary Edition only (diagnostics)", true);
			return;
		}
		const auto mainSite = REL::Relocation<std::uintptr_t>{ REL::ID(36560), 0xE9 }.address();
		if (CheckCall(mainSite, 32174)) {
			MainCull::func = mainSite + 5 + *reinterpret_cast<const std::int32_t*>(mainSite + 1);  // Original zuerst merken (1.0.64)
			std::atomic_thread_fence(std::memory_order_seq_cst);
			trampoline.write_call<5>(mainSite, MainCull::thunk);
			logger::info("Timing installed: main camera culling (ID 32174)");
			Features::Report("Timing: main camera culling", "Zeitmessung: Hauptkamera", true);
		} else {
			Features::Report("Timing: main camera culling", "Zeitmessung: Hauptkamera", false, "call site ID 36560+0xE9 differs");
		}
	}

	bool PrecipTimerInstalled() noexcept { return g_precipInstalled; }
	bool SkylightABRunning() noexcept { return g_abRunning.load(std::memory_order_relaxed) || g_abStartRequested.load(std::memory_order_relaxed); }
	void StartSkylightAB() noexcept
	{
		if (g_precipInstalled && !SkylightABRunning()) {
			g_abStartRequested.store(true);
		}
	}

	void CountSkylight(bool a_culled) noexcept
	{
		(a_culled ? g_abCulled : g_abKept).fetch_add(1, std::memory_order_relaxed);
	}

	namespace
	{
		void BetaOnFrame();
	}

	void OnFrame()
	{
		BetaOnFrame();
		if (g_abStartRequested.exchange(false)) {
			g_abOriginal = Config::skylightingCulling.enabled;
			g_abOriginalPrune = Config::subtreePruning.precip;
			logger::info("[Skylight-AB] start: {} phases of {} s, rotating off / objects only / objects + groups (stand still or walk the same route)", kAbPhases,
				kAbPhaseLength.count());
			g_abRunning.store(true);
			AbBeginPhase(0);
			return;
		}
		if (!g_abRunning.load(std::memory_order_relaxed)) {
			return;
		}
		++g_abPhases[g_abPhase].frames;
		if (std::chrono::steady_clock::now() - g_abPhaseStart >= kAbPhaseLength) {
			AbEndPhase();
			if (g_abPhase + 1 < kAbPhases) {
				AbBeginPhase(g_abPhase + 1);
			} else {
				AbFinish();
			}
		}
	}

	namespace
	{
		constexpr int  kBetaModes = 4;
		constexpr int  kBetaPhases = 12;
		int            g_betaSet = 0;  // 0 = Verdeckung/Schatten-Cache, 1 = Cache der fernen Kaskade
		bool           g_betaOrigFar = false;
		int            Modes() noexcept { return g_betaSet == 1 ? 2 : kBetaModes; }
		int            Phases() noexcept { return g_betaSet == 1 ? 8 : kBetaPhases; }
		constexpr auto kBetaPhaseLength = std::chrono::seconds(15);
		struct BetaPhase
		{
			int           mode = 0;
			std::uint64_t frames = 0;
			double        ms = 0, npc = 0, sunMs = 0, sunDraws = 0;  // Sonnenschatten: CPU-Zeit Render, ausgefuehrte Draws
		};
		std::array<BetaPhase, kBetaPhases>      g_betaPhases;
		int                                     g_betaPhase = -1;
		bool                                    g_betaOrigOcc = false, g_betaOrigCache = false;
		std::atomic<bool>                       g_betaStart{ false }, g_betaRunning{ false };
		std::chrono::steady_clock::time_point   g_betaPhaseStart{}, g_betaLastFrame{};
		constexpr const char*                   kBetaNames[] = { "betas off", "occlusion only", "shadow cache only", "both" };
		constexpr const char*                   kFarNames[] = { "far cache off", "far cache on" };
		const char*                             ModeName(int a_mode) noexcept { return g_betaSet == 1 ? kFarNames[a_mode] : kBetaNames[a_mode]; }

		void BetaBegin(int a_phase)
		{
			g_betaPhase = a_phase;
			const int mode = a_phase % Modes();
			if (g_betaSet == 1) {
				Config::farShadowCache.enabled = mode == 1;
			} else {
				Config::occlusionCull.store(mode == 1 || mode == 3);
				Config::lightShadowCache.enabled = mode == 2 || mode == 3;
			}
			g_betaPhases[a_phase] = BetaPhase{ mode };
			g_betaPhaseStart = std::chrono::steady_clock::now();
		}

		void BetaEnd()
		{
			const auto& p = g_betaPhases[g_betaPhase];
			const double f = p.frames ? static_cast<double>(p.frames) : 1.0;
			logger::info("[Beta-AB] phase {} {}: {} frames | frame avg {:.2f} ms ({:.1f} FPS) | NPC updates/frame {:.1f} | sun shadow draws {:.0f}/frame, CPU {:.3f} ms",
				g_betaPhase + 1, ModeName(p.mode), p.frames, p.ms / f, p.ms > 0 ? 1000.0 * f / p.ms : 0.0, p.npc / f, p.sunDraws / f, p.sunMs / f);
		}

		void BetaFinish()
		{
			std::array<double, kBetaModes> ms{}, fr{}, npc{}, sun{}, draws{};
			for (int i = 0; i < Phases(); ++i) {
				const auto& p = g_betaPhases[i];
				ms[p.mode] += p.ms;
				fr[p.mode] += static_cast<double>(p.frames);
				npc[p.mode] += p.npc;
				sun[p.mode] += p.sunMs;
				draws[p.mode] += p.sunDraws;
			}
			std::string line;
			for (int m = 0; m < Modes(); ++m) {
				const double n = fr[m] > 0 ? fr[m] : 1.0;
				line += std::format("{}{} {:.2f} ms ({:.1f} FPS, NPC {:.1f}, sun shadow draws {:.0f}, CPU {:.3f} ms)", m ? " | " : "", ModeName(m), ms[m] / n,
					ms[m] > 0 ? 1000.0 * fr[m] / ms[m] : 0.0, npc[m] / n, draws[m] / n, sun[m] / n);
			}
			logger::info("[Beta-AB] result: {}", line);
			Config::occlusionCull.store(g_betaOrigOcc);
			Config::lightShadowCache.enabled = g_betaOrigCache;
			Config::farShadowCache.enabled = g_betaOrigFar;
			g_betaRunning.store(false);
			g_betaPhase = -1;
		}

		void BetaOnFrame()
		{
			const auto now = std::chrono::steady_clock::now();
			if (g_betaStart.exchange(false)) {
				g_betaOrigOcc = Config::occlusionCull.load();
				g_betaOrigCache = Config::lightShadowCache.enabled;
				g_betaOrigFar = Config::farShadowCache.enabled;
				logger::info("[Beta-AB] start: {} phases of {} s, rotating {} (stand still or walk the same route)", Phases(), kBetaPhaseLength.count(),
					g_betaSet == 1 ? "far cache off / on" : "betas off / occlusion only / shadow cache only / both");
				g_betaRunning.store(true);
				InstancingAnalysis::TakeSunRenderMs();
				InstancingAnalysis::TakeSunDraws();
				BetaBegin(0);
				g_betaLastFrame = now;
				return;
			}
			if (!g_betaRunning.load(std::memory_order_relaxed)) {
				return;
			}
			const double dt = std::chrono::duration<double, std::milli>(now - g_betaLastFrame).count();
			g_betaLastFrame = now;
			// Pausen (Menue) und Ruckler ueber 250 ms nicht mitzaehlen
			if (dt > 0.0 && dt < 250.0) {
				auto& p = g_betaPhases[g_betaPhase];
				++p.frames;
				p.ms += dt;
				p.npc += Stats::FrameNpcUpdates();
				p.sunMs += InstancingAnalysis::TakeSunRenderMs();
				p.sunDraws += static_cast<double>(InstancingAnalysis::TakeSunDraws());
			} else {
				InstancingAnalysis::TakeSunRenderMs();
				InstancingAnalysis::TakeSunDraws();
			}
			if (now - g_betaPhaseStart >= kBetaPhaseLength) {
				BetaEnd();
				if (g_betaPhase + 1 < Phases()) {
					BetaBegin(g_betaPhase + 1);
				} else {
					BetaFinish();
				}
			}
		}
	}

	void StartBetaAB() noexcept
	{
		if (!BetaABRunning()) {
			g_betaSet = 0;
			g_betaStart.store(true);
		}
	}

	void StartFarAB() noexcept
	{
		if (!BetaABRunning()) {
			g_betaSet = 1;
			g_betaStart.store(true);
		}
	}

	bool BetaABRunning() noexcept { return g_betaRunning.load(std::memory_order_relaxed) || g_betaStart.load(std::memory_order_relaxed); }
}
