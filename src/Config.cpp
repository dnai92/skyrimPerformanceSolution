#include "Config.h"

#include <SimpleIni.h>

namespace Config
{
	void Load()
	{
		constexpr auto path = L"Data/SKSE/Plugins/SkyrimPerf.ini";

		CSimpleIniA ini;
		ini.SetUnicode();
		if (ini.LoadFile(path) < 0) {
			logger::warn("SkyrimPerf.ini nicht gefunden - Defaults aktiv");
		}

		auto& sc = shadowCulling;
		sc.enabled = ini.GetBoolValue("ShadowCulling", "bEnabled", sc.enabled);
		sc.minCascade = static_cast<std::uint32_t>(ini.GetLongValue("ShadowCulling", "iMinCascade", sc.minCascade));
		sc.minDistance = static_cast<float>(ini.GetDoubleValue("ShadowCulling", "fMinDistance", sc.minDistance));
		sc.maxRadius = static_cast<float>(ini.GetDoubleValue("ShadowCulling", "fMaxRadius", sc.maxRadius));
		sc.minAngularSize = static_cast<float>(ini.GetDoubleValue("ShadowCulling", "fMinAngularSize", sc.minAngularSize));
		sc.skipSkinned = ini.GetBoolValue("ShadowCulling", "bSkipSkinned", sc.skipSkinned);

		logger::info("ShadowCulling: {} | ab Kaskade {} | Distanz > {:.0f} | Radius < {:.0f} | Radius/Distanz < {:.3f} | Skinned ausgenommen: {}",
			sc.enabled ? "AN" : "AUS", sc.minCascade, sc.minDistance, sc.maxRadius, sc.minAngularSize, sc.skipSkinned);
	}
}
