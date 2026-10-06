#include "LightGather.h"
#include "Features.h"

#include "Config.h"
#include "Stats.h"

namespace LightGather
{
	namespace
	{
		struct Entry
		{
			const void*                           niLight = nullptr;  // erkennt neu angelegte Lichter an alter Adresse
			RE::NiPoint3                          position;
			float                                 radius = 0.0f;
			std::chrono::steady_clock::time_point time;
		};

		std::mutex                                g_lock;  // Lichtschleife laeuft normal nur im Main-Thread; Sicherheit fuer Sonderfaelle
		std::unordered_map<const void*, Entry>    g_last;  // BSLight -> Stand der letzten Suche
		std::chrono::steady_clock::time_point     g_lastCleanup{};

		// Aufrufstelle in der Lichtschleife: ShadowSceneNode (rcx), BSLight (rdx) -> ID 106342
		struct UpdateLight
		{
			static void thunk(RE::NiNode* a_sceneNode, RE::BSLight* a_light)
			{
				const auto& cfg = Config::lightGather;
				if (!cfg.enabled || !Config::masterEnabled.load(std::memory_order_relaxed) || !a_light || !a_light->light) {
					Gather(a_sceneNode, a_light);
					return;
				}

				const auto  now = std::chrono::steady_clock::now();
				const auto& pos = a_light->worldTranslate;  // hat die Engine direkt vor dem Aufruf gesetzt
				const float radius = a_light->light->GetLightRuntimeData().radius.x;

				{
					std::scoped_lock lock(g_lock);
					if (const auto it = g_last.find(a_light); it != g_last.end()) {
						const auto& e = it->second;
						const float moved = pos.GetDistance(e.position);
						const float dr = std::abs(radius - e.radius);
						const auto  age = std::chrono::duration_cast<std::chrono::milliseconds>(now - e.time).count();
						if (e.niLight == a_light->light.get() && moved < cfg.minMove && dr < cfg.minRadiusChange && age < cfg.maxAgeMs) {
							Stats::Count(Stats::Counter::LightGatherSkipped);
							return;  // bisherige Objektliste des Lichts bleibt gueltig
						}
					}
					g_last[a_light] = { a_light->light.get(), pos, radius, now };

					// verwaiste Eintraege (Licht geloescht) gelegentlich entfernen; Zeiger werden nie dereferenziert
					if (now - g_lastCleanup > std::chrono::seconds(10)) {
						g_lastCleanup = now;
						std::erase_if(g_last, [&](const auto& kv) { return now - kv.second.time > std::chrono::seconds(10); });
					}
				}
				Gather(a_sceneNode, a_light);
			}

			static void Gather(RE::NiNode* a_sceneNode, RE::BSLight* a_light)
			{
				Stats::Count(Stats::Counter::LightGatherCalls);
				Stats::ScopedTimer timer(Stats::Zone::LightGather);
				func(a_sceneNode, a_light);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		// ID 106335 (ShadowSceneNode-Lichtschleife) +0x6E2: call ID 106342 fuer dynamische Punktlichter
		REL::Relocation<std::uintptr_t> site{ REL::ID(106335), 0x6E2 };
		const auto                      addr = site.address();
		const auto                      target = REL::Relocation<std::uintptr_t>{ REL::ID(106342) }.address();
		if (*reinterpret_cast<std::uint8_t*>(addr) != 0xE8 ||
			addr + 5 + *reinterpret_cast<std::int32_t*>(addr + 1) != target) {
			Features::Report("Light assignment throttle", "Licht-Zuordnung drosseln", false, "call site ID 106335+0x6E2 differs (other game version or mod)");
			return;
		}
		UpdateLight::func = SKSE::GetTrampoline().write_call<5>(addr, UpdateLight::thunk);
		logger::info("LightGather: Hook auf Licht-Zuordnung dynamischer Punktlichter installiert");
		Features::Report("Light assignment throttle", "Licht-Zuordnung drosseln", true);
	}
}
