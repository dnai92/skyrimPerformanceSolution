#include "Hooks.h"
#include "Config.h"
#include "ShadowCulling.h"
#include "Stats.h"

namespace
{
	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			Hooks::RegisterEvents();
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::InitInfo info{};
	info.logName = "SkyrimPerf";
	SKSE::Init(a_skse, info);

	logger::info("SkyrimPerf {} geladen (Tracy on-demand, Report alle 10 s)", SKSE::GetPluginVersion().string());

	Stats::Init();
	Config::Load();
	Hooks::Install();
	ShadowCulling::Install();

	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
