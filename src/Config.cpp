#include "Config.h"

#include <SimpleIni.h>

namespace Config
{
	namespace
	{
		constexpr auto kPath = L"Data/SKSE/Plugins/SkyrimPerf.ini";

		std::filesystem::file_time_type g_lastWrite{};
		std::chrono::steady_clock::time_point g_lastCheck{};

		std::filesystem::file_time_type LastWrite() noexcept
		{
			std::error_code ec;
			const auto t = std::filesystem::last_write_time(kPath, ec);
			return ec ? std::filesystem::file_time_type{} : t;
		}
	}

	void ReloadIfChanged()
	{
		const auto now = std::chrono::steady_clock::now();
		if (now - g_lastCheck < 2s) {
			return;
		}
		g_lastCheck = now;
		if (LastWrite() != g_lastWrite) {
			logger::info("SkyrimPerf.ini geaendert - lade neu");
			Load();
		}
	}

	void Load()
	{
		constexpr auto path = kPath;
		g_lastWrite = LastWrite();

		CSimpleIniA ini;
		ini.SetUnicode();
		if (ini.LoadFile(path) < 0) {
			logger::warn("SkyrimPerf.ini nicht gefunden - Defaults aktiv");
		}

		// Werte einzeln lesen und dann setzen; die Culling-Jobs lesen parallel (einzelne Felder, kein Absturz-Risiko)
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
