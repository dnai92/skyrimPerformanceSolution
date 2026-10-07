#include "Hooks.h"
#include "Hotkey.h"
#include "InstancingAnalysis.h"
#include "EngineTimers.h"
#include "LightGather.h"
#include "Menu.h"
#include "Config.h"
#include "Features.h"
#include "ShadowCulling.h"
#include "Stats.h"
#include "TextureStream.h"
#include "WaitProbe.h"

namespace
{
	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (a_msg->type == SKSE::MessagingInterface::kPostLoad) {
			// Messwerkzeug: biegt Warte-Funktionen von FSMP/CBPC/Spiel um. Nur mit Entwickler-Schalter bEngineProbes
			// (INI, beim Start) - im Normalbetrieb sollen fremde Mods nicht durch SPS laufen (Haenger-Analyse Rifton 1.0.6).
			if (!Config::engineProbes) {
				logger::info("WaitProbe: off (developer option bEngineProbes)");
				return;
			}
			WaitProbe::ModuleResult fsmp, cbpc, game;
			WaitProbe::Install(fsmp, cbpc, game);
			logger::info("WaitProbe: FSMP (hdtsmp64.dll) {} - {} import entries redirected | CBPC (cbp.dll) {} - {} redirected | SkyrimSE.exe {} redirected",
				fsmp.found ? "found" : "NOT loaded", fsmp.patched, cbpc.found ? "found" : "NOT loaded", cbpc.patched, game.patched);
		} else if (a_msg->type == SKSE::MessagingInterface::kPreLoadGame) {
			TextureStream::Reset("loading save game");
		} else if (a_msg->type == SKSE::MessagingInterface::kNewGame) {
			TextureStream::Reset("new game");
		} else if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			Hooks::RegisterEvents();
#ifndef SPS_VR
			ShadowCulling::InstallLate();
#endif
			if (Config::engineProbes) {
				EngineTimers::Install();  // nur Zeitmessung; nach Community Shaders (Detours verkettet sich dahinter)
			}
			Hotkey::Register();
			Menu::Register();
			TextureStream::InstallLate();
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::InitInfo info{};
	info.logName = "SPS";
	info.trampoline = true;
	info.trampolineSize = 256;
	SKSE::Init(a_skse, info);

	#ifdef TRACY_ENABLE
	logger::info("SPS {} loaded (developer build with Tracy profiler)", SKSE::GetPluginVersion().string());
#else
	logger::info("SPS {} loaded", SKSE::GetPluginVersion().string());
#endif
	logger::info("Skyrim base address 0x{:X} (for tools/resolve_rva.py)", REL::Module::get().base());

	// Address-Library-IDs: AE ab 1.6.317 stabil (auch 1.7.x, offline verglichen 1.6.640/1.7.104); SE 1.5.97 hat eigene IDs,
	// die fuer alle Eingriffe per Code-Muster ermittelt wurden (RELOCATION_ID). Eingriffe mitten in Funktionen pruefen
	// den Code an ihrer Stelle selbst und schalten bei Abweichung nur sich ab (Features.h). Aeltere SE / VR: inaktiv.
	// Alte Version unter dem frueheren Namen noch installiert? Beide zusammen wuerden dieselben Stellen doppelt patchen.
	if (GetModuleHandleW(L"SkyrimPerf.dll")) {
		logger::error("SkyrimPerf.dll (old name) is also loaded - SPS stays inactive. Please remove the old mod.");
		SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* a_msg) {
			if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
				RE::DebugMessageBox(Menu::IsGerman() ? "Skyrim Performance Solution: Die alte Version (SkyrimPerf) ist noch installiert. Bitte entfernen - SPS bleibt bis dahin inaktiv." :
				                                       "Skyrim Performance Solution: the old version (SkyrimPerf) is still installed. Please remove it - SPS stays inactive until then.");
			}
		});
		return true;
	}

	const auto version = REL::Module::get().version();
#ifdef SPS_VR
	const bool supported = version == REL::Version{ 1, 4, 15, 0 };  // eigener VR-Build: nur Skyrim VR 1.4.15
#else
	const bool supported = version == REL::Version{ 1, 5, 97, 0 } || (version >= REL::Version{ 1, 6, 317, 0 } && version < REL::Version{ 1, 8, 0, 0 });
#endif
	if (!supported) {
#ifdef SPS_VR
		logger::warn("Game version {} is not supported by the SPS VR build (Skyrim VR 1.4.15 only) - SPS stays inactive", version.string());
#else
		logger::warn("Game version {} is not supported (1.5.97 and Anniversary Edition 1.6.x / 1.7.x only) - SPS stays inactive", version.string());
#endif
		SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* a_msg) {
			if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
				const auto v = REL::Module::get().version().string();
				RE::DebugMessageBox((Menu::IsGerman() ? std::format("SPS: Spielversion {} wird nicht unterstützt (nur 1.5.97 und Anniversary Edition 1.6.x / 1.7.x). Das Plugin bleibt inaktiv.", v) :
				                                        std::format("SPS: game version {} is not supported (1.5.97 and Anniversary Edition 1.6.x / 1.7.x only). The plugin stays inactive.", v)).c_str());
			}
		});
		return true;
	}

#ifdef SPS_VR
	logger::info("SPS VR build (beta): texture streaming only - patch sites verified offline against Skyrim VR 1.4.15");
#endif
	if (!Features::TestedVersion() && !REL::Module::IsVR()) {
		if (Features::VerifiedVersion()) {
			logger::info("Game version {}: all patch sites verified offline against this version's game code (in-game testing so far on 1.6.1170)", version.string());
		} else {
			logger::warn("Game version {} not tested - every patch checks its code site at startup, mismatching ones are disabled", version.string());
		}
	}

	Stats::Init();
	Config::Load();
	Hooks::Install();
#ifndef SPS_VR
	ShadowCulling::Install();
	InstancingAnalysis::Install();
	LightGather::Install();
#endif
	TextureStream::Install();

	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
