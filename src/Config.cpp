#include "Config.h"
#include "EngineTimers.h"

#include "TextureStream.h"

#include <SimpleIni.h>

namespace Config
{
	namespace
	{
		constexpr auto kPath = L"Data/SKSE/Plugins/SPS.ini";            // Vorgaben (Mod-Paket, von Vortex verwaltet)
		constexpr auto kUserPath = L"Data/SKSE/Plugins/SPS_User.ini";   // Menue-Einstellungen (vom Plugin geschrieben)

		std::filesystem::file_time_type g_lastWrite{};
		std::filesystem::file_time_type g_lastWriteUser{};
		std::chrono::steady_clock::time_point g_lastCheck{};
		std::atomic<bool> g_dirty{ false };
		std::atomic<bool> g_reset{ false };

		std::filesystem::file_time_type LastWrite(const wchar_t* a_path) noexcept
		{
			std::error_code ec;
			const auto t = std::filesystem::last_write_time(a_path, ec);
			return ec ? std::filesystem::file_time_type{} : t;
		}

		// Nur die im Menue einstellbaren Werte; alles andere kommt weiter aus SPS.ini
		bool SaveUser()
		{
			CSimpleIniA ini;
			ini.SetUnicode();
			// Vorhandene Datei als Grundlage: von Hand eingetragene Schluessel, die das Menue nicht kennt, bleiben erhalten
			// (bis 1.0.57 wurde die Datei aus der festen Liste neu gebaut und alles andere ging verloren)
			ini.LoadFile(kUserPath);
			// Nur Abweichungen von SPS.ini speichern - sonst ueberdecken alte Vorgaben spaeter geaenderte Standards
			// (so blieben bPointLights=true und fMinDistance=2500 aus alten Versionen haengen).
			CSimpleIniA base;
			base.SetUnicode();
			const bool haveBase = base.LoadFile(kPath) >= 0;
			const auto b = [&](const char* s, const char* k, bool v) {
				if (!haveBase || !base.GetValue(s, k) || base.GetBoolValue(s, k, v) != v) {
					ini.SetBoolValue(s, k, v);
				} else {
					ini.Delete(s, k, true);  // wieder Standard
				}
			};
			const auto f = [&](const char* s, const char* k, float v) {
				if (!haveBase || !base.GetValue(s, k) || std::abs(base.GetDoubleValue(s, k, v) - v) > 1e-4 * std::max(1.0, std::abs(double(v)))) {
					ini.SetDoubleValue(s, k, v, nullptr, true);
				} else {
					ini.Delete(s, k, true);
				}
			};
			{
				const long key = static_cast<long>(toggleKey.load());
				if (!haveBase || !base.GetValue("General", "iToggleKey") || base.GetLongValue("General", "iToggleKey", key) != key) {
					ini.SetLongValue("General", "iToggleKey", key);
				} else {
					ini.Delete("General", "iToggleKey", true);
				}
			}
			const auto rule = [&](const char* s, const CullRule& r) {
				b(s, "bEnabled", r.enabled);
				f(s, "fMinDistance", r.minDistance);
				f(s, "fMaxRadius", r.maxRadius);
				f(s, "fMinAngularSize", r.minAngularSize);
			};
			rule("ShadowCulling", shadowCulling);
			f("ShadowCulling", "fMinSunElevation", sunMinElevation.load());
			rule("PointLightShadowCulling", pointLightCulling);
			b("PointLightShadowCulling", "bInteriors", pointLightInteriors.load());
			b("DepthPrepassCulling", "bEnabled", depthPrepassCulling.enabled);
			b("MainViewCulling", "bEnabled", mainViewCulling.enabled);
			b("SkylightingCulling", "bEnabled", skylightingCulling.enabled);
			f("SkylightingCulling", "fMinRadius", skylightingCulling.minRadius);
			b("DecalCulling", "bEnabled", decalCulling.enabled);
			f("DecalCulling", "fMaxDistance", decalCulling.maxDistance);
			f("DecalCulling", "fMaxRadius", decalCulling.maxRadius);
			b("ShadowInstancing", "bEnabled", shadowInstancing.enabled);
			b("ActorShadowCulling", "bEnabled", actorShadowCulling.enabled);
			f("ActorShadowCulling", "fMinDistance", actorShadowCulling.minDistance);
			b("ActorShadowCulling", "bPointLights", actorShadowCulling.pointLights);
			b("ActorPartShadowCulling", "bEnabled", actorPartShadowCulling.enabled);
			b("LightGatherThrottle", "bEnabled", lightGather.enabled);
			b("SubtreePruning", "bEnabled", subtreePruning.enabled);
			b("TextureStream", "bEnabled", textureStream.enabled);
			b("TextureStream", "bAnalysis", textureStream.analysis);
			b("TextureStream", "bLoadReduced", textureStream.loadReduced);
			b("TextureStream", "bAllowWithTextureDownscaler", textureStream.allowWithDownscaler);
			b("TextureStream", "bBudgetMode", textureStream.budgetMode);
			f("TextureStream", "fReserveMB", textureStream.reserveMB);
			b("TextureStream", "bRefill", textureStream.refill);
			f("TextureStream", "fRefillGapMB", textureStream.refillGapMB);
			f("TextureStream", "fRamCacheMB", textureStream.ramCacheMB);
			f("TextureStream", "fSafetyFactor", textureStream.safetyFactor);
			f("TextureStream", "fMinEdge", textureStream.minEdge);
			f("TextureStream", "fMaxEdge", textureStream.maxEdge);
			b("TextureStream", "bStreamClothing", textureStream.streamClothing);
			b("LightShadowCache", "bEnabled", lightShadowCache.enabled);
			b("LightShadowCache", "bAllowWithBottledShaders", lightShadowCache.allowWithBottled);
			b("FarShadowCache", "bEnabled", farShadowCache.enabled);
			b("Occlusion", "bCull", occlusionCull.load());
			f("Occlusion", "fPauseTurnDeg", occlTurnDeg.load());
			f("Occlusion", "fPauseMove", occlMoveUnits.load());
			f("Occlusion", "fMarginPercent", occlMarginPct.load());
			f("Occlusion", "fMarginUnits", occlMarginUnits.load());
			f("Occlusion", "iHiddenInARow", static_cast<float>(occlStreak.load()));
			f("Occlusion", "iMaxTiles", static_cast<float>(occlMaxTiles.load()));
			b("Occlusion", "bNeverCharacters", occlSkipSkinned.load());
			b("Occlusion", "bReproject", occlReproject.load());
			b("Occlusion", "bAllowWithBottledHiZ", occlAllowWithBottled.load());
			b("TextureStream", "bStreamCharacters", textureStream.streamCharacters);
			{
				const long mask = static_cast<long>(::TextureStream::GetDebugInclude());
				if (!haveBase || !base.GetValue("TextureStream", "iIncludeExcluded") || base.GetLongValue("TextureStream", "iIncludeExcluded", mask) != mask) {
					ini.SetLongValue("TextureStream", "iIncludeExcluded", mask);
				} else {
					ini.Delete("TextureStream", "iIncludeExcluded", true);
				}
			}
			b("SubtreePruning", "bSun", subtreePruning.sun);
			b("SubtreePruning", "bPrecipitation", subtreePruning.precip);
			f("LightGatherThrottle", "fMinMove", lightGather.minMove);
			f("LightGatherThrottle", "fMinRadiusChange", lightGather.minRadiusChange);
			f("LightGatherThrottle", "fMaxAgeMs", lightGather.maxAgeMs);
			// Entfernte Funktionen nicht mitschleppen (Kaskaden-Cache 1.0.59, Animations-Drossel/Vorpass-Messung 1.0.60)
			ini.Delete("ShadowCascadeCache", nullptr, true);
			ini.Delete("AnimThrottle", nullptr, true);
			ini.Delete("ShadowStableCascade", "bEnabled", true);
			// Alte Prozent-Schwellen nach der Umrechnung nicht mehr mitschleppen
			if (textureStream.legacyStartPct < 0.0f && textureStream.legacyGapPct < 0.0f) {
				ini.Delete("TextureStream", "fBudgetStartPct", true);
				ini.Delete("TextureStream", "fRefillGapPct", true);
			}
			// Erst in eine Zwischendatei, dann ersetzen: ein Absturz mitten im Schreiben loeschte sonst alle Einstellungen.
			// Ohne WRITE_THROUGH: das erzwungene Durchschreiben auf die Platte hielt den Hauptthread bei HDD/Virenscanner/MO2
			// jedes Mal 10-50 ms auf
			const std::wstring tmp = std::wstring(kUserPath) + L".tmp";
			if (ini.SaveFile(tmp.c_str()) < 0 || !MoveFileExW(tmp.c_str(), kUserPath, MOVEFILE_REPLACE_EXISTING)) {
				logger::warn("Menu settings could not be saved to SPS_User.ini (write protected?)");
				return false;
			}
			g_lastWriteUser = LastWrite(kUserPath);  // eigene Aenderung nicht erneut laden
			return true;
		}
	}

	namespace
	{
		std::atomic<std::chrono::steady_clock::rep> g_lastDirtyTicks{ 0 };
	}

	void MarkDirty() noexcept
	{
		g_lastDirtyTicks.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
		g_dirty.store(true);
	}
	void RequestReset() noexcept { g_reset.store(true); }

	namespace
	{
		std::atomic<std::chrono::steady_clock::rep> g_lastCallTicks{ 0 };
	}

	namespace
	{
		// Hauptmenue und Ladebildschirme praesentieren aus einem eigenen Thread (Test 1.0.61) - Speichern/Laden darf von
		// dort kommen (Menue-Aenderungen im Hauptmenue), aber nie gleichzeitig mit dem Main-Thread
		std::mutex g_reloadLock;
	}

	void ReloadIfIdle()
	{
		const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
		if (now - g_lastCallTicks.load(std::memory_order_relaxed) >= std::chrono::duration_cast<std::chrono::steady_clock::duration>(1s).count()) {
			ReloadIfChanged();
		}
	}

	void ReloadIfChanged()
	{
		g_lastCallTicks.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
		// A/B-Messung laeuft: ihre voruebergehenden Werte duerfen nicht in SPS_User.ini landen und ein Neuladen wuerde die
		// Messung verfaelschen - Speichern/Laden/Zuruecksetzen warten bis zum Ende (die Messung stellt die Werte wieder her)
		if (EngineTimers::BetaABRunning() || EngineTimers::SkylightABRunning()) {
			return;
		}
		std::unique_lock guard(g_reloadLock, std::try_to_lock);
		if (!guard.owns_lock()) {
			return;  // laeuft gerade im anderen Thread
		}
		try {
		if (g_reset.exchange(false)) {
			g_dirty.store(false);
			std::error_code ec;
			std::filesystem::remove(kUserPath, ec);
			logger::info("Menu: SPS_User.ini deleted - defaults from SPS.ini");
			Load();
		}
		const auto now = std::chrono::steady_clock::now();
		if (now - g_lastCheck < 2s) {
			return;
		}
		g_lastCheck = now;
		// erst speichern, wenn seit 1 s nichts mehr geaendert wurde (Regler ziehen schrieb sonst alle 2 s die Datei)
		const auto sinceDirty = now.time_since_epoch().count() - g_lastDirtyTicks.load(std::memory_order_relaxed);
		if (g_dirty.load() && sinceDirty >= std::chrono::duration_cast<std::chrono::steady_clock::duration>(1s).count() && g_dirty.exchange(false)) {
			if (SaveUser()) {
				logger::info("Menu settings saved to SPS_User.ini");
			}
		}
		if (LastWrite(kPath) != g_lastWrite || LastWrite(kUserPath) != g_lastWriteUser) {
			logger::info("SPS.ini / SPS_User.ini changed - reloading");
			Load();
		}
		} catch (const std::exception& e) {
			logger::error("Settings could not be saved/loaded: {}", e.what());  // laeuft im Spiel-Hook: nichts nach aussen werfen
		} catch (...) {
			logger::error("Settings could not be saved/loaded");
		}
	}

	void Load()
	{
		// Umbenennung SkyrimPerf -> SPS (1.0.1): Menue-Einstellungen einmalig uebernehmen
		{
			static bool migrated = false;
			std::error_code ec;
			if (!migrated && !std::filesystem::exists(kUserPath, ec) && std::filesystem::exists(L"Data/SKSE/Plugins/SkyrimPerf_User.ini", ec)) {
				std::filesystem::copy_file(L"Data/SKSE/Plugins/SkyrimPerf_User.ini", kUserPath, ec);
				logger::info("Menu settings migrated from SkyrimPerf_User.ini{}", ec ? " - ERROR: " + ec.message() : "");
				// Alte Datei umbenennen: sonst holte sie nach "Standard laden" (loescht SPS_User.ini) beim naechsten Start
				// die alten Werte zurueck
				if (!ec) {
					std::error_code ec2;
					std::filesystem::rename(L"Data/SKSE/Plugins/SkyrimPerf_User.ini", L"Data/SKSE/Plugins/SkyrimPerf_User.ini.migrated", ec2);
				}
			}
			migrated = true;
		}
		g_lastWrite = LastWrite(kPath);
		g_lastWriteUser = LastWrite(kUserPath);

		CSimpleIniA ini;
		ini.SetUnicode();
		if (ini.LoadFile(kPath) < 0) {
			logger::warn("SPS.ini not found - using defaults");
		}
		// Menue-Werte darueber laden (gleiche Schluessel ersetzen die Vorgaben)
		if (ini.LoadFile(kUserPath) >= 0) {
			logger::info("SPS_User.ini (menu settings) loaded");
		}

		// Werte einzeln setzen; die Culling-Jobs lesen parallel (einzelne Felder, kein Absturz-Risiko)
		// bAnalysis wird nicht gelesen: das Analyse-Protokoll startet immer aus und gilt nur fuer die laufende Sitzung (Menue),
		// damit es niemand versehentlich dauerhaft an laesst (kostet Hauptthread-Zeit). VR hat kein Menue -> dort per INI.
#ifdef SPS_VR
		analysis.store(ini.GetBoolValue("General", "bAnalysis", analysis.load()), std::memory_order_relaxed);
#endif
#ifndef SPS_VR
		static bool firstLoad = true;
		if (firstLoad) {
			engineProbes = ini.GetBoolValue("General", "bEngineProbes", engineProbes);  // nur beim Start (Hooks werden dann installiert)
			firstLoad = false;
		}
#endif
		occlusionCull.store(ini.GetBoolValue("Occlusion", "bCull", occlusionCull.load()), std::memory_order_relaxed);
		occlTurnDeg.store(std::clamp(static_cast<float>(ini.GetDoubleValue("Occlusion", "fPauseTurnDeg", occlTurnDeg.load())), 0.5f, 20.0f), std::memory_order_relaxed);
		occlMoveUnits.store(std::clamp(static_cast<float>(ini.GetDoubleValue("Occlusion", "fPauseMove", occlMoveUnits.load())), 0.0f, 512.0f), std::memory_order_relaxed);
		occlMarginPct.store(std::clamp(static_cast<float>(ini.GetDoubleValue("Occlusion", "fMarginPercent", occlMarginPct.load())), 0.0f, 20.0f), std::memory_order_relaxed);
		occlMarginUnits.store(std::clamp(static_cast<float>(ini.GetDoubleValue("Occlusion", "fMarginUnits", occlMarginUnits.load())), 0.0f, 256.0f), std::memory_order_relaxed);
		occlStreak.store(static_cast<std::uint32_t>(std::clamp(ini.GetDoubleValue("Occlusion", "iHiddenInARow", occlStreak.load()), 1.0, 4.0) + 0.5), std::memory_order_relaxed);
		occlMaxTiles.store(static_cast<std::uint32_t>(std::clamp(ini.GetDoubleValue("Occlusion", "iMaxTiles", occlMaxTiles.load()), 256.0, 36864.0) + 0.5), std::memory_order_relaxed);
		occlSkipSkinned.store(ini.GetBoolValue("Occlusion", "bNeverCharacters", occlSkipSkinned.load()), std::memory_order_relaxed);
		occlReproject.store(ini.GetBoolValue("Occlusion", "bReproject", occlReproject.load()), std::memory_order_relaxed);
		occlAllowWithBottled.store(ini.GetBoolValue("Occlusion", "bAllowWithBottledHiZ", occlAllowWithBottled.load()), std::memory_order_relaxed);
		logger::info("Occlusion (beta): hiding {} | reproject {} | pause from {:.1f} deg / {:.0f} units | margin {:.1f} % + {:.0f} | hidden in {} images in a row | max {} tiles | characters {}", occlusionCull.load() ? "ON" : "OFF", occlReproject.load() ? "on" : "off", occlTurnDeg.load(), occlMoveUnits.load(), occlMarginPct.load(), occlMarginUnits.load(), occlStreak.load(), occlMaxTiles.load(), occlSkipSkinned.load() ? "never" : "allowed");
		toggleKey.store(static_cast<std::uint32_t>(ini.GetLongValue("General", "iToggleKey", toggleKey.load())), std::memory_order_relaxed);

		const auto readRule = [&](const char* a_section, CullRule& a_rule) {
			a_rule.enabled = ini.GetBoolValue(a_section, "bEnabled", a_rule.enabled);
			a_rule.minCascade = static_cast<std::uint32_t>(std::clamp(ini.GetLongValue(a_section, "iMinCascade", a_rule.minCascade), 0L, 8L));
			a_rule.minDistance = static_cast<float>(ini.GetDoubleValue(a_section, "fMinDistance", a_rule.minDistance));
			a_rule.maxRadius = static_cast<float>(ini.GetDoubleValue(a_section, "fMaxRadius", a_rule.maxRadius));
			a_rule.minAngularSize = static_cast<float>(ini.GetDoubleValue(a_section, "fMinAngularSize", a_rule.minAngularSize));
			a_rule.skipSkinned = ini.GetBoolValue(a_section, "bSkipSkinned", a_rule.skipSkinned);
		};
		readRule("ShadowCulling", shadowCulling);
		sunMinElevation.store(static_cast<float>(ini.GetDoubleValue("ShadowCulling", "fMinSunElevation", sunMinElevation.load())), std::memory_order_relaxed);
		readRule("PointLightShadowCulling", pointLightCulling);
		pointLightInteriors.store(ini.GetBoolValue("PointLightShadowCulling", "bInteriors", pointLightInteriors.load()), std::memory_order_relaxed);
		readRule("DepthPrepassCulling", depthPrepassCulling);
		readRule("MainViewCulling", mainViewCulling);

		auto& dec = decalCulling;
		dec.enabled = ini.GetBoolValue("DecalCulling", "bEnabled", dec.enabled);
		dec.maxDistance = static_cast<float>(ini.GetDoubleValue("DecalCulling", "fMaxDistance", dec.maxDistance));
		dec.maxRadius = static_cast<float>(ini.GetDoubleValue("DecalCulling", "fMaxRadius", dec.maxRadius));

		auto& st = stableCascade;
		st.maxAngleDeg = static_cast<float>(ini.GetDoubleValue("ShadowStableCascade", "fMaxAngleDeg", st.maxAngleDeg));
		st.extentStep = std::max(64.0f, static_cast<float>(ini.GetDoubleValue("ShadowStableCascade", "fExtentStep", st.extentStep)));
		st.margin = std::max(0.0f, static_cast<float>(ini.GetDoubleValue("ShadowStableCascade", "fMargin", st.margin)));
		st.extentFactor = std::clamp(static_cast<float>(ini.GetDoubleValue("ShadowStableCascade", "fExtentFactor", st.extentFactor)), 1.0f, 2.0f);
		st.depthStep = std::max(64.0f, static_cast<float>(ini.GetDoubleValue("ShadowStableCascade", "fDepthStep", st.depthStep)));
		logger::info("ShadowStableCascade (with far shadow cache): sun step {:.2f} deg | extent step {:.0f} x{:.2f} | margin {:.0f} | depth step {:.0f}",
			st.maxAngleDeg, st.extentStep, st.extentFactor, st.margin, st.depthStep);

		auto& si = shadowInstancing;
		si.enabled = ini.GetBoolValue("ShadowInstancing", "bEnabled", si.enabled);
		si.verify = ini.GetBoolValue("ShadowInstancing", "bVerify", si.verify);
		si.debugOffsetZ = static_cast<float>(ini.GetDoubleValue("ShadowInstancing", "fDebugOffsetZ", si.debugOffsetZ));
		{
			// Kommagetrennte Hex-Liste, z. B. "C046,C006"
			std::string list = ini.GetValue("ShadowInstancing", "sTechnique", "C046,C045");
			si.techniqueCount = 0;
			std::size_t pos = 0;
			while (pos < list.size() && si.techniqueCount < si.techniques.size()) {
				const auto end = list.find(',', pos);
				const auto item = list.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
				if (!item.empty()) {
					si.techniques[si.techniqueCount++] = static_cast<std::uint32_t>(std::strtoul(item.c_str(), nullptr, 16));
				}
				if (end == std::string::npos) {
					break;
				}
				pos = end + 1;
			}
			si.technique = si.techniqueCount ? si.techniques[0] : 0;
		}
		si.allowTwoSided = ini.GetBoolValue("ShadowInstancing", "bAllowTwoSided", si.allowTwoSided);
		si.debugMode = static_cast<std::uint32_t>(ini.GetLongValue("ShadowInstancing", "iDebugMode", si.debugMode));
		si.minGroup = std::max<std::uint32_t>(2, static_cast<std::uint32_t>(ini.GetLongValue("ShadowInstancing", "iMinGroup", si.minGroup)));
		std::string techs;
		for (std::uint32_t i = 0; i < si.techniqueCount; ++i) {
			techs += std::format("{}{:X}", i ? "," : "", si.techniques[i]);
		}
		logger::info("ShadowInstancing: {} | verify mode {} (offset {:.0f}) | techniques {} | two-sided {} | debug mode {}", si.enabled ? "ON" : "OFF", si.verify, si.debugOffsetZ, techs, si.allowTwoSided, si.debugMode);

		auto& ac = actorShadowCulling;
		ac.enabled = ini.GetBoolValue("ActorShadowCulling", "bEnabled", ac.enabled);
		ac.minDistance = std::max(ActorShadowCulling::kMinAllowed, static_cast<float>(ini.GetDoubleValue("ActorShadowCulling", "fMinDistance", ac.minDistance)));
		ac.pointLights = ini.GetBoolValue("ActorShadowCulling", "bPointLights", ac.pointLights);
		logger::info("ActorShadowCulling: {} | from distance {:.0f} | point lights {}", ac.enabled ? "ON" : "OFF", ac.minDistance, ac.pointLights);

		auto& ap = actorPartShadowCulling;
		ap.enabled = ini.GetBoolValue("ActorPartShadowCulling", "bEnabled", ap.enabled);
		ap.maxRadius = std::clamp(static_cast<float>(ini.GetDoubleValue("ActorPartShadowCulling", "fMaxRadius", ap.maxRadius)), 0.0f, 20.0f);
		ap.minDistance = std::max(200.0f, static_cast<float>(ini.GetDoubleValue("ActorPartShadowCulling", "fMinDistance", ap.minDistance)));
		logger::info("ActorPartShadowCulling: {} | radius < {:.0f} | from distance {:.0f}", ap.enabled ? "ON" : "OFF", ap.maxRadius, ap.minDistance);

		auto& lg = lightGather;
		lg.enabled = ini.GetBoolValue("LightGatherThrottle", "bEnabled", lg.enabled);
		lg.minMove = static_cast<float>(ini.GetDoubleValue("LightGatherThrottle", "fMinMove", lg.minMove));
		lg.minRadiusChange = static_cast<float>(ini.GetDoubleValue("LightGatherThrottle", "fMinRadiusChange", lg.minRadiusChange));
		lg.maxAgeMs = static_cast<float>(ini.GetDoubleValue("LightGatherThrottle", "fMaxAgeMs", lg.maxAgeMs));
		logger::info("LightGatherThrottle: {} | movement >= {:.0f} | radius +-{:.0f} | at the latest after {:.0f} ms", lg.enabled ? "ON" : "OFF", lg.minMove, lg.minRadiusChange, lg.maxAgeMs);

		auto& sp = subtreePruning;
		sp.enabled = ini.GetBoolValue("SubtreePruning", "bEnabled", sp.enabled);
		sp.sun = ini.GetBoolValue("SubtreePruning", "bSun", sp.sun);
		// bPointLights wird bewusst ignoriert (0.17.5): liess Schatten von Feuerschalen/Fackeln verschwinden, Nutzen 1-3 Knoten/Frame.
		// Alte SPS_User.ini (aus 0.16.0, Vorgabe damals an) hatten den Schalter noch gespeichert.
		sp.point = false;
		sp.precip = ini.GetBoolValue("SubtreePruning", "bPrecipitation", sp.precip);
		logger::info("SubtreePruning: {} | sun {} | point light {} | rain/sky {}", sp.enabled ? "ON" : "OFF", sp.sun, sp.point, sp.precip);

		auto& ts = textureStream;
		ts.enabled = ini.GetBoolValue("TextureStream", "bEnabled", ts.enabled);
		ts.analysis = ini.GetBoolValue("TextureStream", "bAnalysis", ts.analysis);
		ts.loadReduced = ini.GetBoolValue("TextureStream", "bLoadReduced", ts.loadReduced);
		ts.allowWithDownscaler = ini.GetBoolValue("TextureStream", "bAllowWithTextureDownscaler", ts.allowWithDownscaler);
		ts.budgetMode = ini.GetBoolValue("TextureStream", "bBudgetMode", ts.budgetMode);
		ts.refill = ini.GetBoolValue("TextureStream", "bRefill", ts.refill);
		ts.refillGapMB = std::clamp(static_cast<float>(ini.GetDoubleValue("TextureStream", "fRefillGapMB", ts.refillGapMB)), 256.0f, 4096.0f);
		ts.ramCacheMB = std::clamp(static_cast<float>(ini.GetDoubleValue("TextureStream", "fRamCacheMB", ts.ramCacheMB)), 0.0f, 16384.0f);
		ts.reserveMB = std::clamp(static_cast<float>(ini.GetDoubleValue("TextureStream", "fReserveMB", ts.reserveMB)), 256.0f, 6144.0f);
		{
			// Im Menue gespeicherte Prozent-Schwelle (bis 1.0.46) einmalig uebernehmen - Umrechnung braucht das VRAM-Budget
			CSimpleIniA user;
			user.SetUnicode();
			if (user.LoadFile(kUserPath) >= 0 && !user.GetValue("TextureStream", "fReserveMB")) {
				if (user.GetValue("TextureStream", "fBudgetStartPct")) {
					ts.legacyStartPct = std::clamp(static_cast<float>(user.GetDoubleValue("TextureStream", "fBudgetStartPct", 85.0)), 0.0f, 100.0f);
				}
				if (user.GetValue("TextureStream", "fRefillGapPct")) {
					ts.legacyGapPct = std::clamp(static_cast<float>(user.GetDoubleValue("TextureStream", "fRefillGapPct", 10.0)), 2.0f, 50.0f);
				}
			}
		}
		// Handeintraege auf die Menue-Bereiche begrenzen (0 oder negativ verkleinerte alles auf die Mindestgroesse)
		ts.safetyFactor = std::clamp(static_cast<float>(ini.GetDoubleValue("TextureStream", "fSafetyFactor", ts.safetyFactor)), 1.0f, 4.0f);
		ts.minEdge = std::clamp(static_cast<float>(ini.GetDoubleValue("TextureStream", "fMinEdge", ts.minEdge)), 256.0f, 4096.0f);
		ts.maxEdge = static_cast<float>(ini.GetDoubleValue("TextureStream", "fMaxEdge", ts.maxEdge));
		// Nur die Menue-Stufen (1K/2K/4K/8K/unbegrenzt) - krumme Werte wirkten, das Menue zeigte aber "unbegrenzt"
		if (ts.maxEdge != 0.0f && ts.maxEdge != 1024.0f && ts.maxEdge != 2048.0f && ts.maxEdge != 4096.0f && ts.maxEdge != 8192.0f) {
			ts.maxEdge = ts.maxEdge < 1024.0f ? (ts.maxEdge > 0.0f ? 1024.0f : 0.0f) : ts.maxEdge < 2048.0f ? 1024.0f : ts.maxEdge < 4096.0f ? 2048.0f : ts.maxEdge < 8192.0f ? 4096.0f : 8192.0f;
		}
		ts.budgetMs = std::clamp(static_cast<float>(ini.GetDoubleValue("TextureStream", "fBudgetMs", ts.budgetMs)), 0.05f, 5.0f);
		ts.exclude = ini.GetValue("TextureStream", "sExclude", ts.exclude.c_str());
		ts.streamClothing = ini.GetBoolValue("TextureStream", "bStreamClothing", ts.streamClothing);
		ts.streamCharacters = ini.GetBoolValue("TextureStream", "bStreamCharacters", ts.streamCharacters);
		::TextureStream::SetDebugInclude(static_cast<std::uint32_t>(ini.GetLongValue("TextureStream", "iIncludeExcluded", 0)) & 0x1F);
		logger::info("TextureStream: {} | report {} | safety factor {:.1f} | min. {:.0f} px | budget {:.2f} ms | clothing/armor {} | bodies/faces/hair {} | excluded: {}",
			ts.enabled ? "ON" : "OFF", ts.analysis, ts.safetyFactor, ts.minEdge, ts.budgetMs, ts.streamClothing, ts.streamCharacters, ts.exclude);

		lightShadowCache.enabled = ini.GetBoolValue("LightShadowCache", "bEnabled", lightShadowCache.enabled);
		lightShadowCache.debugMode = static_cast<float>(ini.GetDoubleValue("LightShadowCache", "iDebugMode", lightShadowCache.debugMode));
		lightShadowCache.allowWithBottled = ini.GetBoolValue("LightShadowCache", "bAllowWithBottledShaders", lightShadowCache.allowWithBottled);
		logger::info("LightShadowCache: {}", lightShadowCache.enabled ? "ON" : "OFF");
		farShadowCache.enabled = ini.GetBoolValue("FarShadowCache", "bEnabled", farShadowCache.enabled);
		logger::info("FarShadowCache (beta): {}", farShadowCache.enabled ? "ON" : "OFF");

		auto& sky = skylightingCulling;
		sky.enabled = ini.GetBoolValue("SkylightingCulling", "bEnabled", sky.enabled);
		sky.minRadius = static_cast<float>(ini.GetDoubleValue("SkylightingCulling", "fMinRadius", sky.minRadius));

		const auto& sc = shadowCulling;
		logger::info("ShadowCulling: {} | from cascade {} | distance > {:.0f} | radius < {:.0f} | radius/distance < {:.3f} | skinned excluded: {} | only above sun elevation {:.0f} deg",
			sc.enabled ? "ON" : "OFF", sc.minCascade, sc.minDistance, sc.maxRadius, sc.minAngularSize, sc.skipSkinned, sunMinElevation.load());
		const auto& pc = pointLightCulling;
		logger::info("PointLightShadowCulling: {} | distance > {:.0f} | radius < {:.0f} | radius/distance < {:.3f} | skinned excluded: {}",
			pc.enabled ? "ON" : "OFF", pc.minDistance, pc.maxRadius, pc.minAngularSize, pc.skipSkinned);
		logger::info("SkylightingCulling: {} | min. radius {:.0f}", sky.enabled ? "ON" : "OFF", sky.minRadius);
		logger::info("DecalCulling: {} | from distance {:.0f} | radius < {:.0f}", dec.enabled ? "ON" : "OFF", dec.maxDistance, dec.maxRadius);
		const auto& mc = mainViewCulling;
		logger::info("MainViewCulling: {} | distance > {:.0f} | radius < {:.0f} | radius/distance < {:.4f} | skinned excluded: {}",
			mc.enabled ? "ON" : "OFF", mc.minDistance, mc.maxRadius, mc.minAngularSize, mc.skipSkinned);
		const auto& dc = depthPrepassCulling;
		logger::info("DepthPrepassCulling: {} | distance > {:.0f} | radius < {:.0f} | radius/distance < {:.3f} | skinned excluded: {}",
			dc.enabled ? "ON" : "OFF", dc.minDistance, dc.maxRadius, dc.minAngularSize, dc.skipSkinned);
	}
}
