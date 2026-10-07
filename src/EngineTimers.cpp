#include "EngineTimers.h"
#include "Features.h"

#include "DetourHelper.h"
#include "Stats.h"

namespace EngineTimers
{
	namespace
	{
		// SetupMask wird mit Community Shaders nicht ueber die Aufrufstelle in ID 26175 aufgerufen (Zeit dort 0),
		// daher am Funktionsanfang per Detours (verkettet sich mit evtl. vorhandenen Detours anderer Mods)
		struct PrecipSetupMask
		{
			static void thunk(void* a_precipitation)
			{
				Stats::ScopedTimer timer(Stats::Zone::PrecipMask);
				func(a_precipitation);
			}
			static inline void (*func)(void*) = nullptr;
		};

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
			logger::info("Timing installed: Precipitation::SetupMask (rain/sky map, Detours)");
			Features::Report("Timing: rain/sky map", "Zeitmessung: Regen-/Himmelskarte", true);
		} else {
			Features::Report("Timing: rain/sky map", "Zeitmessung: Regen-/Himmelskarte", false, std::format("Detours error {}", err));
		}
		// Nur AE: Aufrufstelle fuer SE 1.5.97 nicht ermittelt (reine Zeitmessung)
		if (REL::Module::IsSE()) {
			Features::Report("Timing: main camera culling", "Zeitmessung: Hauptkamera", false, "Anniversary Edition only (diagnostics)", true);
			return;
		}
		const auto mainSite = REL::Relocation<std::uintptr_t>{ REL::ID(36560), 0xE9 }.address();
		if (CheckCall(mainSite, 32174)) {
			MainCull::func = trampoline.write_call<5>(mainSite, MainCull::thunk);
			logger::info("Timing installed: main camera culling (ID 32174)");
			Features::Report("Timing: main camera culling", "Zeitmessung: Hauptkamera", true);
		} else {
			Features::Report("Timing: main camera culling", "Zeitmessung: Hauptkamera", false, "call site ID 36560+0xE9 differs");
		}
	}
}
