#include "EngineTimers.h"

#include "Stats.h"

namespace EngineTimers
{
	namespace
	{
		struct PrecipSetupMask
		{
			static void thunk(void* a_precipitation)
			{
				Stats::ScopedTimer timer(Stats::Zone::PrecipMask);
				func(a_precipitation);
			}
			static inline REL::Relocation<decltype(thunk)> func;
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
		const auto precipSite = REL::Relocation<std::uintptr_t>{ REL::ID(26175), 0xBB }.address();
		if (CheckCall(precipSite, 26183)) {
			PrecipSetupMask::func = trampoline.write_call<5>(precipSite, PrecipSetupMask::thunk);
			logger::info("Zeitmessung installiert: Precipitation::SetupMask (Regen/Sky-Karte)");
		} else {
			logger::warn("Zeitmessung Regen/Sky-Karte: Aufrufstelle passt nicht - nicht installiert");
		}
		const auto mainSite = REL::Relocation<std::uintptr_t>{ REL::ID(36560), 0xE9 }.address();
		if (CheckCall(mainSite, 32174)) {
			MainCull::func = trampoline.write_call<5>(mainSite, MainCull::thunk);
			logger::info("Zeitmessung installiert: Hauptkamera-Culling (ID 32174)");
		} else {
			logger::warn("Zeitmessung Hauptkamera-Culling: Aufrufstelle passt nicht - nicht installiert");
		}
	}
}
