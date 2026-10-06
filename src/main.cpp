#include "Hooks.h"
#include "Hotkey.h"
#include "InstancingAnalysis.h"
#include "EngineTimers.h"
#include "LightGather.h"
#include "Menu.h"
#include "Config.h"
#include "ShadowCulling.h"
#include "Stats.h"
#include "TextureStream.h"
#include "WaitProbe.h"

namespace
{
	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (a_msg->type == SKSE::MessagingInterface::kPostLoad) {
			WaitProbe::ModuleResult fsmp, cbpc, game;
			WaitProbe::Install(fsmp, cbpc, game);
			logger::info("WaitProbe: FSMP (hdtsmp64.dll) {} - {} Import-Eintraege umgebogen | CBPC (cbp.dll) {} - {} umgebogen | SkyrimSE.exe {} umgebogen",
				fsmp.found ? "gefunden" : "NICHT geladen", fsmp.patched, cbpc.found ? "gefunden" : "NICHT geladen", cbpc.patched, game.patched);
		} else if (a_msg->type == SKSE::MessagingInterface::kPreLoadGame) {
			TextureStream::Reset("Spielstand wird geladen");
		} else if (a_msg->type == SKSE::MessagingInterface::kNewGame) {
			TextureStream::Reset("neues Spiel");
		} else if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			Hooks::RegisterEvents();
			ShadowCulling::InstallLate();
			EngineTimers::Install();  // nach Community Shaders (Detours verkettet sich dahinter)
			Hotkey::Register();
			Menu::Register();
			TextureStream::InstallLate();
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::InitInfo info{};
	info.logName = "SkyrimPerf";
	info.trampoline = true;
	info.trampolineSize = 256;
	SKSE::Init(a_skse, info);

	logger::info("SkyrimPerf {} geladen (Tracy on-demand, Report alle 10 s)", SKSE::GetPluginVersion().string());
	logger::info("Skyrim-Basisadresse 0x{:X} (fuer tools/resolve_rva.py)", REL::Module::get().base());

	// Engine-Eingriffe benutzen AE-Adressen (Address-Library-IDs sind innerhalb der AE 1.6.x stabil). Eingriffe mitten in
	// Funktionen pruefen den Code an ihrer Stelle selbst und schalten bei Abweichung nur sich ab (Features.h).
	// SE 1.5.x / VR haben andere IDs -> dort gar nichts installieren.
	const auto version = REL::Module::get().version();
	if (version < REL::Version{ 1, 6, 317, 0 } || version >= REL::Version{ 1, 7, 0, 0 }) {
		logger::warn("Spielversion {} wird nicht unterstuetzt (nur Anniversary Edition 1.6.x) - SkyrimPerf bleibt inaktiv", version.string());
		SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* a_msg) {
			if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
				const auto v = REL::Module::get().version().string();
				RE::DebugMessageBox((Menu::IsGerman() ? std::format("SkyrimPerf: Spielversion {} wird nicht unterstützt (nur Anniversary Edition 1.6.x). Das Plugin bleibt inaktiv.", v) :
				                                        std::format("SkyrimPerf: game version {} is not supported (Anniversary Edition 1.6.x only). The plugin stays inactive.", v)).c_str());
			}
		});
		return true;
	}

	if (version != REL::Version{ 1, 6, 1170, 0 }) {
		logger::warn("Spielversion {} ist nicht getestet (getestet: 1.6.1170) - Eingriffe pruefen den Code selbst, abweichende werden abgeschaltet", version.string());
	}

	Stats::Init();
	Config::Load();
	Hooks::Install();
	ShadowCulling::Install();
	InstancingAnalysis::Install();
	LightGather::Install();
	TextureStream::Install();

	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
