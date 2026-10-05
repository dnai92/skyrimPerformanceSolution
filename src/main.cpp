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
		} else if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			Hooks::RegisterEvents();
			ShadowCulling::InstallLate();
			EngineTimers::Install();  // nach Community Shaders (Detours verkettet sich dahinter)
			Hotkey::Register();
			Menu::Register();
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
