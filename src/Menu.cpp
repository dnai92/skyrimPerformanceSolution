#include "Menu.h"

#include "Config.h"

#include "third_party/SKSEMenuFramework.h"

namespace Menu
{
	namespace
	{
		// Sprache folgt der Spielsprache (Skyrim.ini sLanguage:General): GERMAN -> Deutsch, sonst Englisch
		bool g_german = false;

		bool DetectGerman()
		{
			const char* lang = nullptr;
			if (const auto ini = RE::INISettingCollection::GetSingleton()) {
				if (const auto setting = ini->GetSetting("sLanguage:General")) {
					lang = setting->GetString();
				}
			}
			std::string l = lang ? lang : "";
			std::ranges::transform(l, l.begin(), [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
			logger::info("Spielsprache: {} -> Menue {}", l.empty() ? "(unbekannt)" : l, l == "GERMAN" ? "Deutsch" : "Englisch");
			return l == "GERMAN";
		}

		// Text je nach Sprache
		const char* T(const char* a_en, const char* a_de) noexcept { return g_german ? a_de : a_en; }

		void Tip(const char* a_text);

		// ---- Taste fuer den Hauptschalter: ImGui-Taste -> Windows-VK -> DirectInput-Scancode (wie das Spiel ihn meldet) ----
		extern "C" __declspec(dllimport) unsigned int __stdcall MapVirtualKeyW(unsigned int a_code, unsigned int a_mapType);

		unsigned int VirtualKeyOf(ImGuiMCP::ImGuiKey a_key) noexcept
		{
			using namespace ImGuiMCP;
			const int k = a_key;
			if (k >= ImGuiKey_0 && k <= ImGuiKey_9) return '0' + (k - ImGuiKey_0);
			if (k >= ImGuiKey_A && k <= ImGuiKey_Z) return 'A' + (k - ImGuiKey_A);
			if (k >= ImGuiKey_F1 && k <= ImGuiKey_F24) return 0x70 + (k - ImGuiKey_F1);
			if (k >= ImGuiKey_Keypad0 && k <= ImGuiKey_Keypad9) return 0x60 + (k - ImGuiKey_Keypad0);
			switch (k) {
			case ImGuiKey_Tab: return 0x09;
			case ImGuiKey_LeftArrow: return 0x25;
			case ImGuiKey_UpArrow: return 0x26;
			case ImGuiKey_RightArrow: return 0x27;
			case ImGuiKey_DownArrow: return 0x28;
			case ImGuiKey_PageUp: return 0x21;
			case ImGuiKey_PageDown: return 0x22;
			case ImGuiKey_Home: return 0x24;
			case ImGuiKey_End: return 0x23;
			case ImGuiKey_Insert: return 0x2D;
			case ImGuiKey_Delete: return 0x2E;
			case ImGuiKey_Backspace: return 0x08;
			case ImGuiKey_Space: return 0x20;
			case ImGuiKey_Enter: return 0x0D;
			case ImGuiKey_LeftCtrl: return 0xA2;
			case ImGuiKey_RightCtrl: return 0xA3;
			case ImGuiKey_LeftShift: return 0xA0;
			case ImGuiKey_RightShift: return 0xA1;
			case ImGuiKey_LeftAlt: return 0xA4;
			case ImGuiKey_RightAlt: return 0xA5;
			case ImGuiKey_Apostrophe: return 0xDE;
			case ImGuiKey_Comma: return 0xBC;
			case ImGuiKey_Minus: return 0xBD;
			case ImGuiKey_Period: return 0xBE;
			case ImGuiKey_Slash: return 0xBF;
			case ImGuiKey_Semicolon: return 0xBA;
			case ImGuiKey_Equal: return 0xBB;
			case ImGuiKey_LeftBracket: return 0xDB;
			case ImGuiKey_Backslash: return 0xDC;
			case ImGuiKey_RightBracket: return 0xDD;
			case ImGuiKey_GraveAccent: return 0xC0;
			case ImGuiKey_CapsLock: return 0x14;
			case ImGuiKey_ScrollLock: return 0x91;
			case ImGuiKey_Pause: return 0x13;
			default: return 0;
			}
		}

		// Scancode im DirectInput-Format (erweiterte Tasten wie Pfeile/Bild auf: + 0x80); 0 = nicht zuordenbar
		std::uint32_t DikOf(ImGuiMCP::ImGuiKey a_key) noexcept
		{
			const auto vk = VirtualKeyOf(a_key);
			if (!vk) {
				return 0;
			}
			const auto sc = MapVirtualKeyW(vk, 4);  // MAPVK_VK_TO_VSC_EX: 0xE0xx bei erweiterten Tasten
			if (!sc) {
				return 0;
			}
			// MapVirtualKey meldet "erweitert" (E0) nicht zuverlaessig: Bild auf kam als 0x49 = Ziffernblock 9 zurueck.
			// Diese Tasten teilen sich den Scancode mit dem Ziffernblock und sind immer erweitert.
			bool extended = (sc & 0xFF00) == 0xE000 || (sc & 0xFF00) == 0xE100;
			switch (vk) {
			case 0x21: case 0x22: case 0x23: case 0x24:  // Bild auf/ab, Ende, Pos1
			case 0x25: case 0x26: case 0x27: case 0x28:  // Pfeile
			case 0x2D: case 0x2E:                        // Einfg, Entf
			case 0xA3: case 0xA5:                        // Strg rechts, AltGr
				extended = true;
				break;
			default:
				break;
			}
			return (sc & 0x7F) | (extended ? 0x80u : 0u);
		}

		std::string KeyName(std::uint32_t a_dik)
		{
			char buf[64]{};
			const std::int32_t lparam = static_cast<std::int32_t>(((a_dik & 0x7F) << 16) | ((a_dik & 0x80) ? (1u << 24) : 0u));
			if (REX::W32::GetKeyNameTextA(lparam, buf, sizeof(buf)) > 0) {
				return buf;
			}
			return std::format("0x{:X}", a_dik);
		}

		bool g_capturingKey = false;

		void HotkeyPicker()
		{
			const auto current = Config::toggleKey.load();
			const auto label = g_capturingKey ? std::string(T("Press a key... (Esc = cancel)", "Taste drücken ... (Esc = abbrechen)")) : KeyName(current);
			if (ImGuiMCP::Button((label + "###hotkey").c_str())) {
				g_capturingKey = true;
			}
			ImGuiMCP::SameLine();
			ImGuiMCP::TextUnformatted(T("Hotkey for the master switch", "Taste für den Hauptschalter"));
			Tip(T("Click, then press the new key. Works immediately and is saved.", "Anklicken, dann die neue Taste drücken. Wirkt sofort und wird gespeichert."));
			if (!g_capturingKey) {
				return;
			}
			ImGuiMCP::SetNextFrameWantCaptureKeyboard(true);
			if (ImGuiMCP::IsKeyPressed(ImGuiMCP::ImGuiKey_Escape, false)) {
				g_capturingKey = false;
			} else if (const auto key = ImGuiMCPComponents::Detail::FindPressedKey(); key != ImGuiMCP::ImGuiKey_None) {
				if (const auto dik = DikOf(key)) {
					Config::toggleKey.store(dik);
					Config::MarkDirty();
					logger::info("Menue: Hotkey jetzt {} (0x{:X})", KeyName(dik), dik);
				}
				g_capturingKey = false;
			}
		}

		void Tip(const char* a_text)
		{
			if (ImGuiMCP::IsItemHovered()) {
				ImGuiMCP::SetTooltip("%s", a_text);
			}
		}

		// Werte werden direkt in Config geaendert (die Spiel-Threads lesen einzelne Felder; kein Absturz-Risiko,
		// wie beim INI-Neuladen). Gespeichert wird verzoegert im Main-Thread.
		bool Toggle(const char* a_label, bool& a_value, const char* a_tip)
		{
			const bool changed = ImGuiMCP::Checkbox(a_label, &a_value);
			Tip(a_tip);
			if (changed) {
				Config::MarkDirty();
			}
			return changed;
		}

		bool Slider(const char* a_label, float& a_value, float a_min, float a_max, const char* a_format, const char* a_tip)
		{
			const bool changed = ImGuiMCP::SliderFloat(a_label, &a_value, a_min, a_max, a_format);
			Tip(a_tip);
			if (changed) {
				Config::MarkDirty();
			}
			return changed;
		}

		bool AtomicToggle(const char* a_label, std::atomic<bool>& a_value, const char* a_tip, bool a_save = true)
		{
			bool       v = a_value.load();
			const bool changed = ImGuiMCP::Checkbox(a_label, &v);
			Tip(a_tip);
			if (changed) {
				a_value.store(v);
				if (a_save) {
					Config::MarkDirty();
				}
			}
			return changed;
		}

		void RuleControls(const char* a_id, Config::CullRule& a_rule, float a_maxDistance)
		{
			ImGuiMCP::PushID(a_id);
			Slider(T("Min. distance", "Min. Entfernung"), a_rule.minDistance, 0.0f, a_maxDistance, "%.0f",
				T("Objects closer to the camera always keep their shadow (game units, 70 = 1 m).", "Näher an der Kamera behalten Objekte immer ihren Schatten (Spieleinheiten, 70 = 1 m)."));
			Slider(T("Max. object radius", "Max. Objektradius"), a_rule.maxRadius, 10.0f, 500.0f, "%.0f",
				T("Bigger objects always keep their shadow.", "Größere Objekte behalten immer ihren Schatten."));
			Slider(T("Min. apparent size", "Min. scheinbare Größe"), a_rule.minAngularSize, 0.001f, 0.1f, "%.3f",
				T("Radius / distance. Objects that appear smaller than this lose their shadow. Higher = more culling.",
					"Radius / Entfernung. Objekte, die kleiner erscheinen, verlieren ihren Schatten. Höher = mehr Culling."));
			ImGuiMCP::PopID();
		}

		void __stdcall RenderOverview()
		{
			ImGuiMCP::SeparatorText(T("Master switch", "Hauptschalter"));
			AtomicToggle(T("All optimizations active", "Alle Optimierungen aktiv"), Config::masterEnabled,
				T("Turns every optimization on/off at once (same as the hotkey, Page Up by default). Not saved - starts ON.",
					"Schaltet alle Optimierungen auf einmal an/aus (wie die Taste, Standard Bild auf). Wird nicht gespeichert - startet AN."),
				false);
			HotkeyPicker();
			ImGuiMCP::TextWrapped("%s", T("Use this switch (or the hotkey) to compare FPS and look with and without SkyrimPerf. The 10-second report in SkyrimPerf.log shows the numbers.",
											"Mit diesem Schalter (oder der Taste) FPS und Bild mit und ohne SkyrimPerf vergleichen. Der 10-Sekunden-Bericht in SkyrimPerf.log zeigt die Zahlen."));

			ImGuiMCP::SeparatorText(T("Optimizations", "Optimierungen"));
			Toggle(T("Sun shadow culling", "Sonnenschatten-Culling"), Config::shadowCulling.enabled,
				T("Small, far objects do not cast sun shadows.", "Kleine, ferne Objekte werfen keinen Sonnenschatten."));
			Toggle(T("Torch / point light shadow culling", "Fackel-/Punktlichtschatten-Culling"), Config::pointLightCulling.enabled,
				T("Small, far objects do not cast shadows from torches and fires.", "Kleine, ferne Objekte werfen keinen Schatten von Fackeln und Feuern."));
			Toggle(T("Character shadow culling", "Figurenschatten-Culling"), Config::actorShadowCulling.enabled,
				T("Characters far away do not cast shadows.", "Weit entfernte Figuren werfen keinen Schatten."));
			Toggle(T("Skylighting culling (Community Shaders)", "Skylighting-Culling (Community Shaders)"), Config::skylightingCulling.enabled,
				T("Small objects are left out of the skylighting occlusion map.", "Kleine Objekte werden in der Skylighting-Verdeckungskarte weggelassen."));
			Toggle(T("Decal culling", "Decal-Culling"), Config::decalCulling.enabled,
				T("Small decals (footprints, blood, dirt) far away are not drawn.", "Kleine Decals (Fußspuren, Blut, Schmutz) in der Ferne werden nicht gezeichnet."));
			Toggle(T("Shadow instancing", "Schatten-Instancing"), Config::shadowInstancing.enabled,
				T("Draws identical simple meshes in sun shadows with one draw call.", "Zeichnet gleiche einfache Meshes im Sonnenschatten mit einem Draw-Call."));
			Toggle(T("Light assignment throttle", "Licht-Zuordnung drosseln"), Config::lightGather.enabled,
				T("Moving lights (torches, flickering lights) only search for the objects they light when they really moved.",
					"Bewegte Lichter (Fackeln, flackernde Lichter) suchen die beleuchteten Objekte nur neu, wenn sie sich wirklich bewegt haben."));
			Toggle(T("Subtree pruning", "Teilbäume überspringen"), Config::subtreePruning.enabled,
				T("Skips whole groups of objects in the shadow and skylighting passes when the group as a whole is already small and far enough to be culled. Same result, less work.",
					"Überspringt ganze Objektgruppen im Schatten- und Skylighting-Durchlauf, wenn schon die Gruppe als Ganzes klein und weit genug ist. Gleiches Ergebnis, weniger Arbeit."));
			Toggle(T("Texture streaming", "Texture-Streaming"), Config::textureStream.enabled,
				T("Textures of far objects are shrunk in VRAM and reloaded at full size when you come closer.",
					"Texturen ferner Objekte werden im VRAM verkleinert und bei Annäherung in voller Größe neu geladen."));

			ImGuiMCP::SeparatorText(T("Experimental (known side effects)", "Experimentell (bekannte Nebenwirkungen)"));
			Toggle(T("Depth pre-pass culling", "Tiefenvorpass-Culling"), Config::depthPrepassCulling.enabled,
				T("Can make whole objects disappear. Default OFF.", "Kann ganze Objekte verschwinden lassen. Standard AUS."));
			Toggle(T("Main view micro culling", "Mikro-Culling Hauptansicht"), Config::mainViewCulling.enabled,
				T("Tiny far objects are not drawn at all - can pop in. Default OFF.", "Winzige ferne Objekte werden gar nicht gezeichnet - können aufploppen. Standard AUS."));
			Toggle(T("Far shadow cascade cache", "Cache ferne Schattenkaskade"), Config::cascadeCache.enabled,
				T("Far sun shadows only every 2nd frame - flickers with a low sun. Default OFF.", "Ferne Sonnenschatten nur jeden 2. Frame - flackert bei tiefer Sonne. Standard AUS."));

			ImGuiMCP::SeparatorText(T("Settings", "Einstellungen"));
			if (ImGuiMCP::Button(T("Reset to defaults", "Auf Standard zurücksetzen"))) {
				Config::RequestReset();
			}
			Tip(T("Deletes SkyrimPerf_User.ini and reloads the defaults from SkyrimPerf.ini (takes up to 2 s).",
				"Löscht SkyrimPerf_User.ini und lädt die Standardwerte aus SkyrimPerf.ini (dauert bis zu 2 s)."));
			AtomicToggle(T("Analysis logging (costs performance)", "Analyse-Protokoll (kostet Leistung)"), Config::analysis,
				T("Extra diagnostics in SkyrimPerf.log. Costs 2-4 ms per frame - only for troubleshooting.",
					"Zusätzliche Diagnose in SkyrimPerf.log. Kostet 2-4 ms pro Frame - nur zur Fehlersuche."));
		}

		void __stdcall RenderShadows()
		{
			ImGuiMCP::SeparatorText(T("Sun shadow culling", "Sonnenschatten-Culling"));
			Toggle(T("Enabled##sun", "Aktiv##sun"), Config::shadowCulling.enabled,
				T("Small, far objects do not cast sun shadows.", "Kleine, ferne Objekte werfen keinen Sonnenschatten."));
			RuleControls("sun", Config::shadowCulling, 5000.0f);
			float elevation = Config::sunMinElevation.load();
			if (Slider(T("Only above sun elevation (deg)", "Nur ab Sonnenhöhe (Grad)"), elevation, 0.0f, 60.0f, "%.0f",
					T("With a low sun shadows get long - below this elevation nothing is culled.", "Bei tiefer Sonne werden Schatten lang - darunter wird nichts weggelassen."))) {
				Config::sunMinElevation.store(elevation);
			}

			ImGuiMCP::SeparatorText(T("Torch / point light shadow culling", "Fackel-/Punktlichtschatten-Culling"));
			Toggle(T("Enabled##point", "Aktiv##point"), Config::pointLightCulling.enabled,
				T("Small, far objects do not cast shadows from torches and fires.", "Kleine, ferne Objekte werfen keinen Schatten von Fackeln und Feuern."));
			RuleControls("point", Config::pointLightCulling, 5000.0f);

			ImGuiMCP::SeparatorText(T("Character shadow culling", "Figurenschatten-Culling"));
			Toggle(T("Enabled##actor", "Aktiv##actor"), Config::actorShadowCulling.enabled,
				T("Characters far away do not cast shadows.", "Weit entfernte Figuren werfen keinen Schatten."));
			Slider(T("From distance##actor", "Ab Entfernung##actor"), Config::actorShadowCulling.minDistance, Config::ActorShadowCulling::kMinAllowed, 8000.0f, "%.0f",
				T("Characters farther away than this lose their shadow (3500 = ~50 m default, 2100 = ~30 m minimum).",
					"Weiter entfernte Figuren verlieren ihren Schatten (3500 = ~50 m Standard, 2100 = ~30 m Minimum)."));
			Toggle(T("Also torch shadows##actor", "Auch Fackelschatten##actor"), Config::actorShadowCulling.pointLights,
				T("Also leave out character shadows cast by torches and fires.", "Auch Figurenschatten von Fackeln und Feuern weglassen."));

			ImGuiMCP::SeparatorText(T("Shadow instancing", "Schatten-Instancing"));
			Toggle(T("Enabled##inst", "Aktiv##inst"), Config::shadowInstancing.enabled,
				T("Draws identical simple meshes in sun shadows with one draw call.", "Zeichnet gleiche einfache Meshes im Sonnenschatten mit einem Draw-Call."));
		}

		void __stdcall RenderScene()
		{
			ImGuiMCP::SeparatorText(T("Light assignment throttle", "Licht-Zuordnung drosseln"));
			Toggle(T("Enabled##light", "Aktiv##light"), Config::lightGather.enabled,
				T("Moving lights only search for the objects they light when they really moved.", "Bewegte Lichter suchen die beleuchteten Objekte nur neu, wenn sie sich wirklich bewegt haben."));
			Slider(T("Min. movement", "Min. Bewegung"), Config::lightGather.minMove, 1.0f, 64.0f, "%.0f",
				T("A light searches again when it moved at least this far (game units). Flickering moves lights only a little.",
					"Ein Licht sucht neu, wenn es sich mindestens so weit bewegt hat (Spieleinheiten). Flackern bewegt Lichter nur wenig."));
			Slider(T("Min. radius change", "Min. Radiusänderung"), Config::lightGather.minRadiusChange, 1.0f, 128.0f, "%.0f",
				T("... or its radius changed by this much ...", "... oder sein Radius sich um so viel geändert hat ..."));
			Slider(T("Max. age (ms)", "Max. Alter (ms)"), Config::lightGather.maxAgeMs, 16.0f, 1000.0f, "%.0f",
				T("... or the last search is older than this. Lower = characters walking past get lit sooner.",
					"... oder die letzte Suche älter ist. Niedriger = vorbeilaufende Figuren werden früher beleuchtet."));

			ImGuiMCP::SeparatorText(T("Subtree pruning", "Teilbäume überspringen"));
			Toggle(T("Enabled##prune", "Aktiv##prune"), Config::subtreePruning.enabled,
				T("Skips whole groups of objects in culling passes when the group as a whole already meets the culling rule.",
					"Überspringt ganze Objektgruppen, wenn schon die Gruppe als Ganzes die Culling-Regel erfüllt."));
			Toggle(T("Sun / moon shadows##prune", "Sonnen-/Mondschatten##prune"), Config::subtreePruning.sun,
				T("Apply in the sun (moon at night) shadow pass.", "Im Schattendurchlauf der Sonne (nachts Mond) anwenden."));
			Toggle(T("Rain / skylighting map##prune", "Regen-/Skylighting-Karte##prune"), Config::subtreePruning.precip,
				T("Apply in the precipitation / skylighting occlusion pass.", "Im Niederschlags-/Skylighting-Verdeckungsdurchlauf anwenden."));

			ImGuiMCP::SeparatorText(T("Texture streaming", "Texture-Streaming"));
			Toggle(T("Downscale distant textures##ts", "Ferne Texturen verkleinern##ts"), Config::textureStream.enabled,
				T("Textures of far objects (also behind you) are shrunk in VRAM and reloaded at full size from disk when you come closer. Files are never changed. Off = everything goes back to full size.",
					"Texturen ferner Objekte (auch hinter dir) werden im VRAM verkleinert und bei Annäherung in voller Größe von der Platte neu geladen. Dateien werden nie verändert. Aus = alles wieder in voller Größe."));
			Toggle(T("Load at remembered size##ts", "Gleich in gemerkter Größe laden##ts"), Config::textureStream.loadReduced,
				T("The size a texture needed last time (also in earlier sessions) is used directly when the game loads it again. Less VRAM peak and loading when entering areas.",
					"Die zuletzt benötigte Größe einer Textur (auch aus früheren Sitzungen) wird direkt beim Laden verwendet. Weniger VRAM-Spitze und Laden beim Betreten von Gebieten."));
			Toggle(T("Report in log##ts", "Bericht im Log##ts"), Config::textureStream.analysis,
				T("Every 10 s: textures managed, downscaled, VRAM saved, reloads. Results in SkyrimPerf.log.",
					"Alle 10 s: verwaltete und verkleinerte Texturen, gesparter VRAM, Neuladungen. Ergebnisse in SkyrimPerf.log."));
			Slider(T("Safety factor##ts", "Sicherheitsfaktor##ts"), Config::textureStream.safetyFactor, 1.0f, 4.0f, "%.1f",
				T("Needed texture size = size on screen x this factor. Higher = sharper, less saving.",
					"Benötigte Texturgröße = Größe auf dem Bildschirm x Faktor. Höher = schärfer, weniger Ersparnis."));
			if (Slider(T("Min. size (px)##ts", "Min. Größe (px)##ts"), Config::textureStream.minEdge, 256.0f, 4096.0f, "%.0f",
					T("Textures are never shrunk below this edge length. Higher = safer, less saving.",
						"Texturen werden nie unter diese Kantenlänge verkleinert. Höher = sicherer, weniger Ersparnis."))) {
				float p = 256.0f;
				while (p * 1.5f < Config::textureStream.minEdge) {
					p *= 2.0f;
				}
				Config::textureStream.minEdge = p;
			}

			ImGuiMCP::SeparatorText(T("Skylighting culling (Community Shaders)", "Skylighting-Culling (Community Shaders)"));
			Toggle(T("Enabled##sky", "Aktiv##sky"), Config::skylightingCulling.enabled,
				T("Small objects are left out of the skylighting occlusion map.", "Kleine Objekte werden in der Skylighting-Verdeckungskarte weggelassen."));
			Slider(T("Min. object radius##sky", "Min. Objektradius##sky"), Config::skylightingCulling.minRadius, 32.0f, 512.0f, "%.0f",
				T("Objects smaller than this are left out.", "Kleinere Objekte werden weggelassen."));

			ImGuiMCP::SeparatorText(T("Decal culling", "Decal-Culling"));
			Toggle(T("Enabled##decal", "Aktiv##decal"), Config::decalCulling.enabled,
				T("Small decals far away are not drawn.", "Kleine Decals in der Ferne werden nicht gezeichnet."));
			Slider(T("From distance##decal", "Ab Entfernung##decal"), Config::decalCulling.maxDistance, 500.0f, 5000.0f, "%.0f",
				T("Decals farther away than this are not drawn. Below 1500 leaves can disappear.",
					"Weiter entfernte Decals werden nicht gezeichnet. Unter 1500 können Blätter verschwinden."));
			Slider(T("Max. decal radius##decal", "Max. Decal-Radius##decal"), Config::decalCulling.maxRadius, 10.0f, 500.0f, "%.0f",
				T("Only decals smaller than this are affected.", "Nur kleinere Decals sind betroffen."));
		}
	}

	void Register()
	{
		if (!SKSEMenuFramework::IsInstalled()) {
			logger::warn("SKSE Menu Framework nicht installiert - kein Menue, Einstellungen nur ueber die INI");
			return;
		}
		g_german = DetectGerman();
		SKSEMenuFramework::SetSection("SkyrimPerf");
		SKSEMenuFramework::AddSectionItem(T("Overview", "Übersicht"), RenderOverview);
		SKSEMenuFramework::AddSectionItem(T("Shadows", "Schatten"), RenderShadows);
		SKSEMenuFramework::AddSectionItem(T("Lights and scene", "Licht und Szene"), RenderScene);
		logger::info("Menue im SKSE Menu Framework registriert (Version {:.1f})", SKSEMenuFramework::GetMenuFrameworkVersion());
	}
}
