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

		// Werte einzeln setzen; die Culling-Jobs lesen parallel (einzelne Felder, kein Absturz-Risiko)
		toggleKey.store(static_cast<std::uint32_t>(ini.GetLongValue("General", "iToggleKey", toggleKey.load())), std::memory_order_relaxed);

		const auto readRule = [&](const char* a_section, CullRule& a_rule) {
			a_rule.enabled = ini.GetBoolValue(a_section, "bEnabled", a_rule.enabled);
			a_rule.minCascade = static_cast<std::uint32_t>(ini.GetLongValue(a_section, "iMinCascade", a_rule.minCascade));
			a_rule.minDistance = static_cast<float>(ini.GetDoubleValue(a_section, "fMinDistance", a_rule.minDistance));
			a_rule.maxRadius = static_cast<float>(ini.GetDoubleValue(a_section, "fMaxRadius", a_rule.maxRadius));
			a_rule.minAngularSize = static_cast<float>(ini.GetDoubleValue(a_section, "fMinAngularSize", a_rule.minAngularSize));
			a_rule.skipSkinned = ini.GetBoolValue(a_section, "bSkipSkinned", a_rule.skipSkinned);
		};
		readRule("ShadowCulling", shadowCulling);
		sunMinElevation.store(static_cast<float>(ini.GetDoubleValue("ShadowCulling", "fMinSunElevation", sunMinElevation.load())), std::memory_order_relaxed);
		readRule("PointLightShadowCulling", pointLightCulling);
		readRule("DepthPrepassCulling", depthPrepassCulling);
		readRule("MainViewCulling", mainViewCulling);

		auto& dec = decalCulling;
		dec.enabled = ini.GetBoolValue("DecalCulling", "bEnabled", dec.enabled);
		dec.maxDistance = static_cast<float>(ini.GetDoubleValue("DecalCulling", "fMaxDistance", dec.maxDistance));
		dec.maxRadius = static_cast<float>(ini.GetDoubleValue("DecalCulling", "fMaxRadius", dec.maxRadius));

		auto& cc = cascadeCache;
		cc.enabled = ini.GetBoolValue("ShadowCascadeCache", "bEnabled", cc.enabled);
		cc.cascade = static_cast<std::uint32_t>(ini.GetLongValue("ShadowCascadeCache", "iCascade", cc.cascade));
		cc.interval = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(ini.GetLongValue("ShadowCascadeCache", "iInterval", cc.interval)));
		cc.freezeMatrix = ini.GetBoolValue("ShadowCascadeCache", "bFreezeMatrix", cc.freezeMatrix);
		cc.noClear = ini.GetBoolValue("ShadowCascadeCache", "bNoClear", cc.noClear);
		cc.freezeCamera = ini.GetBoolValue("ShadowCascadeCache", "bFreezeCamera", cc.freezeCamera);
		cc.freezeSplits = ini.GetBoolValue("ShadowCascadeCache", "bFreezeSplits", cc.freezeSplits);
		cc.skipDraws = ini.GetBoolValue("ShadowCascadeCache", "bSkipDraws", cc.skipDraws);
		cc.requireSameProjection = ini.GetBoolValue("ShadowCascadeCache", "bRequireSameProjection", cc.requireSameProjection);
		cc.projectionEpsilon = static_cast<float>(ini.GetDoubleValue("ShadowCascadeCache", "fProjectionEpsilon", cc.projectionEpsilon));
		cc.restoreShadowmap = ini.GetBoolValue("ShadowCascadeCache", "bRestoreShadowmap", cc.restoreShadowmap);
		cc.restoreVolumetric = ini.GetBoolValue("ShadowCascadeCache", "bRestoreVolumetric", cc.restoreVolumetric);
		logger::info("ShadowCascadeCache: {} | ab Kaskade {} | neu zeichnen jeden {}. Frame | Matrix {} | Kamera {} | Grenzen {} | Clear aus {} | Kopie Schatten {} | Kopie Volumetric {} | Draws weglassen {} | nur bei gleicher Projektion {} (eps {})",
			cc.enabled ? "AN" : "AUS", cc.cascade, cc.interval, cc.freezeMatrix, cc.freezeCamera, cc.freezeSplits, cc.noClear, cc.restoreShadowmap, cc.restoreVolumetric, cc.skipDraws, cc.requireSameProjection, cc.projectionEpsilon);

		auto& si = shadowInstancing;
		si.enabled = ini.GetBoolValue("ShadowInstancing", "bEnabled", si.enabled);
		si.verify = ini.GetBoolValue("ShadowInstancing", "bVerify", si.verify);
		si.debugOffsetZ = static_cast<float>(ini.GetDoubleValue("ShadowInstancing", "fDebugOffsetZ", si.debugOffsetZ));
		si.technique = static_cast<std::uint32_t>(std::strtoul(ini.GetValue("ShadowInstancing", "sTechnique", "C046"), nullptr, 16));
		si.minGroup = std::max<std::uint32_t>(2, static_cast<std::uint32_t>(ini.GetLongValue("ShadowInstancing", "iMinGroup", si.minGroup)));
		logger::info("ShadowInstancing: {} | Pruefmodus {} (Versatz {:.0f}) | Technik {:X} | ab {} gleichen Meshes", si.enabled ? "AN" : "AUS", si.verify, si.debugOffsetZ, si.technique, si.minGroup);

		auto& sky = skylightingCulling;
		sky.enabled = ini.GetBoolValue("SkylightingCulling", "bEnabled", sky.enabled);
		sky.minRadius = static_cast<float>(ini.GetDoubleValue("SkylightingCulling", "fMinRadius", sky.minRadius));

		const auto& sc = shadowCulling;
		logger::info("ShadowCulling: {} | ab Kaskade {} | Distanz > {:.0f} | Radius < {:.0f} | Radius/Distanz < {:.3f} | Skinned ausgenommen: {} | erst ab Sonnenhoehe {:.0f} Grad",
			sc.enabled ? "AN" : "AUS", sc.minCascade, sc.minDistance, sc.maxRadius, sc.minAngularSize, sc.skipSkinned, sunMinElevation.load());
		const auto& pc = pointLightCulling;
		logger::info("PointLightShadowCulling: {} | Distanz > {:.0f} | Radius < {:.0f} | Radius/Distanz < {:.3f} | Skinned ausgenommen: {}",
			pc.enabled ? "AN" : "AUS", pc.minDistance, pc.maxRadius, pc.minAngularSize, pc.skipSkinned);
		logger::info("SkylightingCulling: {} | Mindestradius {:.0f}", sky.enabled ? "AN" : "AUS", sky.minRadius);
		logger::info("DecalCulling: {} | ab Distanz {:.0f} | Radius < {:.0f}", dec.enabled ? "AN" : "AUS", dec.maxDistance, dec.maxRadius);
		const auto& mc = mainViewCulling;
		logger::info("MainViewCulling: {} | Distanz > {:.0f} | Radius < {:.0f} | Radius/Distanz < {:.4f} | Skinned ausgenommen: {}",
			mc.enabled ? "AN" : "AUS", mc.minDistance, mc.maxRadius, mc.minAngularSize, mc.skipSkinned);
		const auto& dc = depthPrepassCulling;
		logger::info("DepthPrepassCulling: {} | Distanz > {:.0f} | Radius < {:.0f} | Radius/Distanz < {:.3f} | Skinned ausgenommen: {}",
			dc.enabled ? "AN" : "AUS", dc.minDistance, dc.maxRadius, dc.minAngularSize, dc.skipSkinned);
	}
}
