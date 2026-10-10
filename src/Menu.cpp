#include "Menu.h"

#include "Config.h"
#include "Features.h"
#include "Occlusion.h"
#include "ShadowCulling.h"
#include "TextureStream.h"

#include "MenuApi.h"

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
			logger::info("Game language: {} -> menu {}", l.empty() ? "(unknown)" : l, l == "GERMAN" ? "German" : "English");
			return l == "GERMAN";
		}

		// Bereich im SKSE Menu Framework (links in der Liste)
		const std::string kSection = "Skyrim Performance Solution/";

		// Text je nach Sprache
		const char* T(const char* a_en, const char* a_de) noexcept { return g_german ? a_de : a_en; }

		void Tip(const char* a_text);

		// Meldung oben links im Spiel (Debug::Notification der Engine, SE 52050 / AE 52933 - Funktion offline geprueft)
		void Notify(const char* a_text)
		{
			using func_t = void(const char*, const char*, bool);
			static REL::Relocation<func_t> func{ RELOCATION_ID(52050, 52933) };
			func(a_text, nullptr, true);
		}

		// ---- Taste fuer den Hauptschalter: ImGui-Taste -> Windows-VK -> DirectInput-Scancode (wie das Spiel ihn meldet) ----
		extern "C" __declspec(dllimport) unsigned int __stdcall MapVirtualKeyW(unsigned int a_code, unsigned int a_mapType);

		unsigned int VirtualKeyOf(MenuApi::Key a_key) noexcept
		{
			using namespace MenuApi;
			const int k = a_key;
			if (k >= Key_0 && k <= Key_9) return '0' + (k - Key_0);
			if (k >= Key_A && k <= Key_Z) return 'A' + (k - Key_A);
			if (k >= Key_F1 && k <= Key_F24) return 0x70 + (k - Key_F1);
			if (k >= Key_Keypad0 && k <= Key_Keypad9) return 0x60 + (k - Key_Keypad0);
			switch (k) {
			case Key_Tab: return 0x09;
			case Key_LeftArrow: return 0x25;
			case Key_UpArrow: return 0x26;
			case Key_RightArrow: return 0x27;
			case Key_DownArrow: return 0x28;
			case Key_PageUp: return 0x21;
			case Key_PageDown: return 0x22;
			case Key_Home: return 0x24;
			case Key_End: return 0x23;
			case Key_Insert: return 0x2D;
			case Key_Delete: return 0x2E;
			case Key_Backspace: return 0x08;
			case Key_Space: return 0x20;
			case Key_Enter: return 0x0D;
			case Key_LeftCtrl: return 0xA2;
			case Key_RightCtrl: return 0xA3;
			case Key_LeftShift: return 0xA0;
			case Key_RightShift: return 0xA1;
			case Key_LeftAlt: return 0xA4;
			case Key_RightAlt: return 0xA5;
			case Key_Apostrophe: return 0xDE;
			case Key_Comma: return 0xBC;
			case Key_Minus: return 0xBD;
			case Key_Period: return 0xBE;
			case Key_Slash: return 0xBF;
			case Key_Semicolon: return 0xBA;
			case Key_Equal: return 0xBB;
			case Key_LeftBracket: return 0xDB;
			case Key_Backslash: return 0xDC;
			case Key_RightBracket: return 0xDD;
			case Key_GraveAccent: return 0xC0;
			case Key_CapsLock: return 0x14;
			case Key_ScrollLock: return 0x91;
			case Key_Pause: return 0x13;
			default: return 0;
			}
		}

		// Scancode im DirectInput-Format (erweiterte Tasten wie Pfeile/Bild auf: + 0x80); 0 = nicht zuordenbar
		std::uint32_t DikOf(MenuApi::Key a_key) noexcept
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
			const auto label = g_capturingKey ? std::string(T("Press a key... (Esc = cancel)", "Taste drücken ... (Esc = abbrechen)")) :
			                   current == 0   ? std::string(T("No key", "Keine Taste")) :
			                                    KeyName(current);
			if (MenuApi::Button((label + "###hotkey").c_str())) {
				g_capturingKey = true;
			}
			MenuApi::SameLine();
			MenuApi::TextUnformatted(T("Hotkey for the master switch", "Taste für den Hauptschalter"));
			Tip(T("Click, then press the new key. Works immediately and is saved.", "Anklicken, dann die neue Taste drücken. Wirkt sofort und wird gespeichert."));
			if (current != 0 && !g_capturingKey) {
				MenuApi::SameLine();
				if (MenuApi::Button(T("No key###hotkeynone", "Keine Taste###hotkeynone"))) {
					Config::toggleKey.store(0);
					Config::MarkDirty();
					logger::info("Menu: hotkey removed");
				}
				Tip(T("The master switch then only works here in the menu.", "Der Hauptschalter geht dann nur noch hier im Menü."));
			}
			if (!g_capturingKey) {
				return;
			}
			MenuApi::SetNextFrameWantCaptureKeyboard(true);
			if (MenuApi::IsKeyPressed(MenuApi::Key_Escape, false)) {
				g_capturingKey = false;
			} else if (const auto key = MenuApi::FindPressedKey(); key != MenuApi::Key_None) {
				if (const auto dik = DikOf(key)) {
					Config::toggleKey.store(dik);
					Config::MarkDirty();
					logger::info("Menu: hotkey now {} (0x{:X})", KeyName(dik), dik);
				}
				g_capturingKey = false;
			}
		}

		void Tip(const char* a_text)
		{
			if (MenuApi::IsItemHovered()) {
				MenuApi::SetTooltip("%s", a_text);
			}
		}

		// Werte werden direkt in Config geaendert (die Spiel-Threads lesen einzelne Felder; kein Absturz-Risiko,
		// wie beim INI-Neuladen). Gespeichert wird verzoegert im Main-Thread.
		bool Toggle(const char* a_label, bool& a_value, const char* a_tip)
		{
			const bool changed = MenuApi::Checkbox(a_label, &a_value);
			Tip(a_tip);
			if (changed) {
				Config::MarkDirty();
			}
			return changed;
		}

		bool Slider(const char* a_label, float& a_value, float a_min, float a_max, const char* a_format, const char* a_tip)
		{
			const bool changed = MenuApi::SliderFloat(a_label, &a_value, a_min, a_max, a_format);
			Tip(a_tip);
			if (changed) {
				Config::MarkDirty();
			}
			return changed;
		}

		bool AtomicToggle(const char* a_label, std::atomic<bool>& a_value, const char* a_tip, bool a_save = true)
		{
			bool       v = a_value.load();
			const bool changed = MenuApi::Checkbox(a_label, &v);
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
			MenuApi::PushID(a_id);
			Slider(T("Min. distance", "Min. Entfernung"), a_rule.minDistance, 0.0f, a_maxDistance, "%.0f",
				T("Objects closer to the camera always keep their shadow (game units, 70 = 1 m).", "Näher an der Kamera behalten Objekte immer ihren Schatten (Spieleinheiten, 70 = 1 m)."));
			Slider(T("Max. object radius", "Max. Objektradius"), a_rule.maxRadius, 10.0f, 500.0f, "%.0f",
				T("Bigger objects always keep their shadow.", "Größere Objekte behalten immer ihren Schatten."));
			Slider(T("Min. apparent size", "Min. scheinbare Größe"), a_rule.minAngularSize, 0.001f, 0.1f, "%.3f",
				T("Radius / distance. Objects that appear smaller than this lose their shadow. Higher = more culling.",
					"Radius / Entfernung. Objekte, die kleiner erscheinen, verlieren ihren Schatten. Höher = mehr wird weggelassen."));
			MenuApi::PopID();
		}

		void __stdcall RenderOverview()
		{
			MenuApi::SeparatorText(T("Master switch", "Hauptschalter"));
			AtomicToggle(T("All optimizations active", "Alle Optimierungen aktiv"), Config::masterEnabled,
				T("Turns every optimization on/off at once (same as the hotkey, Page Up by default). Not saved - starts ON.",
					"Schaltet alle Optimierungen auf einmal an/aus (wie die Taste, Standard Bild auf). Wird nicht gespeichert - startet AN."),
				false);
			HotkeyPicker();
			MenuApi::TextWrapped("%s", T("Use this switch (or the hotkey) to compare FPS and look with and without SPS. The report in SPS.log (every minute) shows the numbers.",
											"Mit diesem Schalter (oder der Taste) FPS und Bild mit und ohne SPS vergleichen. Der Bericht in SPS.log (jede Minute) zeigt die Zahlen."));

			// Status der Engine-Eingriffe (andere Spielversion / andere Mod an derselben Stelle)
			{
				const auto list = Features::List();
				const auto off = std::ranges::count_if(list, [](const Features::Entry& e) { return !e.active; });
				const auto ver = REL::Module::get().version().string();
				if (off == 0) {
					MenuApi::TextWrapped("%s", std::format("{} {} - {}", T("Game version", "Spielversion"), ver,
													Features::TestedVersion()   ? T("tested, all functions active.", "getestet, alle Funktionen aktiv.") :
													Features::VerifiedVersion() ? T("code verified, all functions active.", "Code geprüft, alle Funktionen aktiv.") :
																				  T("not tested, but all functions passed their code check.", "nicht getestet, aber alle Funktionen haben ihre Code-Prüfung bestanden.")).c_str());
				} else {
					MenuApi::TextWrapped("%s", std::format("{} {}: {}", T("Game version", "Spielversion"), ver,
													T("these functions were switched off because the game code differs (see SPS.log):",
														"diese Funktionen wurden abgeschaltet, weil der Spielcode abweicht (siehe SPS.log):")).c_str());
					for (const auto& e : list) {
						if (!e.active) {
							MenuApi::BulletText("%s", (g_german ? e.nameDe : e.name).c_str());
						}
					}
				}
			}

			MenuApi::SeparatorText(T("Optimizations", "Optimierungen"));
			Toggle(T("Sun shadow culling", "Sonnenschatten kleiner Objekte weglassen"), Config::shadowCulling.enabled,
				T("Small, far objects do not cast sun shadows.", "Kleine, ferne Objekte werfen keinen Sonnenschatten."));
			Toggle(T("Torch / point light shadow culling", "Fackelschatten kleiner Objekte weglassen"), Config::pointLightCulling.enabled,
				T("Small, far objects do not cast shadows from torches and fires.", "Kleine, ferne Objekte werfen keinen Schatten von Fackeln und Feuern."));
			Toggle(T("Character shadow culling", "Figurenschatten in der Ferne weglassen"), Config::actorShadowCulling.enabled,
				T("Characters far away do not cast shadows.", "Weit entfernte Figuren werfen keinen Schatten."));
			Toggle(T("Skylighting culling (Community Shaders)", "Himmelslicht: kleine Objekte weglassen (Community Shaders)"), Config::skylightingCulling.enabled,
				T("Small objects are left out of the skylighting occlusion map.", "Kleine Objekte werden in der Himmelslicht-Karte weggelassen."));
			Toggle(T("Decal culling", "Ferne Bodendetails weglassen"), Config::decalCulling.enabled,
				T("Small decals (footprints, blood, dirt) far away are not drawn.", "Kleine Bodendetails (Fußspuren, Blut, Schmutz) in der Ferne werden nicht gezeichnet."));
			Toggle(T("Light assignment throttle", "Licht-Zuordnung drosseln"), Config::lightGather.enabled,
				T("Moving lights (torches, flickering lights) only search for the objects they light when they really moved.",
					"Bewegte Lichter (Fackeln, flackernde Lichter) suchen die beleuchteten Objekte nur neu, wenn sie sich wirklich bewegt haben."));
			Toggle(T("Subtree pruning", "Teilbäume überspringen"), Config::subtreePruning.enabled,
				T("Skips whole groups of objects in the shadow and skylighting passes when the group as a whole is already small and far enough to be culled. Same result, less work.",
					"Überspringt ganze Objektgruppen im Schatten- und Himmelslicht-Durchlauf, wenn schon die Gruppe als Ganzes klein und weit genug ist. Gleiches Ergebnis, weniger Arbeit."));
			Toggle(T("Texture streaming", "Textur-Streaming"), Config::textureStream.enabled,
				T("Textures of far objects are shrunk in VRAM and reloaded at full size when you come closer.",
					"Texturen ferner Objekte werden im VRAM verkleinert und bei Annäherung in voller Größe neu geladen."));

			MenuApi::SeparatorText(T("Beta (off by default)", "Beta (standardmäßig aus)"));
			MenuApi::TextWrapped("%s", T("These work, but not perfectly on every setup yet. Switch off again if something looks wrong.", "Diese funktionieren, aber noch nicht auf jedem System perfekt. Bei Bildfehlern wieder ausschalten."));
			Toggle(T("Shadow cache for static lights (BETA)", "Schatten fester Lichter zwischenspeichern (BETA)"), Config::lightShadowCache.enabled,
				T("Shadows of lights that do not move are kept, only characters and moving things are redrawn. Mainly for interiors. With some lighting mods (e.g. Lux) shadows near fires can pulse.",
					"Schatten von Lichtern, die sich nicht bewegen, werden aufbewahrt, nur Figuren und Bewegliches werden neu gezeichnet. Vor allem für Innenräume. Mit manchen Lichtmods (z. B. Lux) können Schatten an Feuern pulsieren."));
			Toggle(T("Shadow instancing (BETA)", "Gleiche Schatten bündeln (BETA)"), Config::shadowInstancing.enabled,
				T("Draws identical simple meshes in sun shadows with one draw call. Small gain. Switch off again if shadows or meshes flicker.", "Zeichnet gleiche einfache Objekte im Sonnenschatten in einem Durchgang. Kleiner Gewinn. Bei flackernden Schatten oder Objekten wieder ausschalten."));
			Toggle(T("Main view micro culling (BETA)", "Winzige Objekte im Bild weglassen (BETA)"), Config::mainViewCulling.enabled,
				T("Tiny far objects are not drawn at all. They can pop in.", "Winzige ferne Objekte werden gar nicht gezeichnet. Sie können aufploppen."));
			Toggle(T("Calmer far shadows (BETA)", "Ruhigere ferne Schatten (BETA)"), Config::stableCascade.enabled,
				T("Holds the far sun shadow cascade still instead of realigning it every frame, so far shadows shimmer less when you move. They follow the sun in tiny steps. No performance gain.",
					"Hält die ferne Sonnenschatten-Kaskade ruhig, statt sie jedes Frame neu auszurichten, ferne Schatten flimmern beim Bewegen weniger. Sie folgen der Sonne in kleinen Schritten. Kein Leistungsgewinn."));
			AtomicToggle(T("Hide occluded objects (BETA)", "Verdeckte Objekte weglassen (BETA)"), Config::occlusionCull,
				T("Objects completely hidden behind walls, houses or terrain are not drawn. Fine-tuning on the page Beta.", "Objekte, die ganz hinter Mauern, Häusern oder Gelände verdeckt sind, werden nicht gezeichnet. Feineinstellung auf der Seite Beta."));

			MenuApi::SeparatorText(T("Settings", "Einstellungen"));
			if (MenuApi::Button(T("Reset to defaults", "Auf Standard zurücksetzen"))) {
				Config::RequestReset();
			}
			Tip(T("Deletes SPS_User.ini and reloads the defaults from SPS.ini (takes up to 2 s).",
				"Löscht SPS_User.ini und lädt die Standardwerte aus SPS.ini (dauert bis zu 2 s)."));
			if (Config::analysis.load()) {
				MenuApi::TextWrapped("%s", T("Analysis logging is active (page Debug) and costs 2-4 ms per frame. Switch it off for normal play and FPS comparisons.",
												"Das Analyse-Protokoll ist aktiv (Seite Debug) und kostet 2-4 ms pro Bild. Für normales Spielen und FPS-Vergleiche ausschalten."));
			}
		}

		// Debug: nichts davon wird gespeichert, nach jedem Spielstart ist alles aus
		void DebugInclude(const char* a_label, std::uint32_t a_bit, const char* a_tip)
		{
			const auto mask = TextureStream::GetDebugInclude();
			bool       v = (mask & a_bit) != 0;
			if (MenuApi::Checkbox(a_label, &v)) {
				TextureStream::SetDebugInclude(v ? (mask | a_bit) : (mask & ~a_bit));
			}
			Tip(a_tip);
		}

		void __stdcall RenderDebug()
		{
			MenuApi::TextWrapped("%s", T("For troubleshooting and tests. Nothing on this page is saved, everything is off again after a restart.",
											"Für Fehlersuche und Tests. Nichts auf dieser Seite wird gespeichert, nach einem Neustart ist alles wieder aus."));
			MenuApi::SeparatorText(T("Logging", "Protokoll"));
			if (AtomicToggle(T("Analysis logging (costs performance)", "Analyse-Protokoll (kostet Leistung)"), Config::analysis,
					T("Extra diagnostics in SPS.log (incl. detailed texture streaming report every 10 s). Costs 2-4 ms per frame - only for troubleshooting. Not saved - always starts OFF.",
						"Zusätzliche Diagnose in SPS.log (inkl. ausführlichem Textur-Streaming-Bericht alle 10 s). Kostet 2-4 ms pro Frame - nur zur Fehlersuche. Wird nicht gespeichert - startet immer AUS."),
					false) &&
				Config::analysis.load()) {
				Notify(T("SPS: analysis logging ON - costs performance (off again after restart)", "SPS: Analyse-Protokoll AN - kostet Leistung (nach Neustart wieder aus)"));
			}
			if (Config::analysis.load()) {
				MenuApi::TextWrapped("%s", T("Analysis logging is active and costs 2-4 ms per frame. Switch it off for normal play and FPS comparisons.",
												"Das Analyse-Protokoll ist aktiv und kostet 2-4 ms pro Bild. Für normales Spielen und FPS-Vergleiche ausschalten."));
			}

			MenuApi::SeparatorText(T("Developer tests", "Entwickler-Tests"));
			Toggle(T("Depth pre-pass culling", "Tiefenvorpass: kleine Objekte weglassen"), Config::depthPrepassCulling.enabled,
				T("For testing only, can make whole objects disappear. Default OFF (saved).", "Nur zum Testen, kann ganze Objekte verschwinden lassen. Standard AUS (wird gespeichert)."));
			if (MenuApi::Button(T("Log shadow cascades (300 frames)##cc", "Schattenkaskaden protokollieren (300 Frames)##cc"))) {
				ShadowCulling::RequestCascadeDump(300);
			}
			Tip(T("Writes how the game aligns the sun shadow cascades in each frame to SPS.log.", "Schreibt pro Frame, wie das Spiel die Sonnenschatten-Kaskaden ausrichtet, in SPS.log."));

			MenuApi::SeparatorText(T("Hiding occluded objects (diagnostics)", "Verdeckte Objekte weglassen (Diagnose)"));
			AtomicToggle(T("Measure hidden objects##occ", "Verdeckte Objekte messen##occ"), Config::occlusionProbe,
				T("Counts how many objects in the main view are hidden behind the depth of the previous frame and writes it to SPS.log every minute. Nothing is left out.",
					"Zählt, wie viele Objekte im Hauptbild hinter der Tiefe des vorigen Bildes verdeckt sind, und schreibt das jede Minute in SPS.log. Es wird nichts weggelassen."),
				false);
			{
				bool mainDepth = Config::occlusionDepthSource.load() == 1;
				if (MenuApi::Checkbox(T("Use final depth buffer instead of pre-pass copy##occ", "Fertigen Tiefenpuffer statt Kopie nach dem Vorpass nutzen##occ"), &mainDepth)) {
					Config::occlusionDepthSource.store(mainDepth ? 1 : 0);
				}
				Tip(T("Cross-check for the occlusion test: which depth image the test uses.", "Gegentest für den Verdeckungs-Test: welches Tiefenbild verwendet wird."));
			}
			if (MenuApi::Button(T("Save depth image##occ", "Tiefenbild speichern##occ"))) {
				Occlusion::RequestDepthDump();
			}
			Tip(T("Writes the current depth image of the occlusion test next to SPS.log (SPS_OcclusionDepth_N.pgm). Take a screenshot at the same time.",
				"Schreibt das aktuelle Tiefenbild des Verdeckungs-Tests neben SPS.log (SPS_OcclusionDepth_N.pgm). Gleichzeitig einen Screenshot machen."));
		}

		// Beta: fertige, aber noch wenig erprobte Funktionen, Standard AUS
		void __stdcall RenderBeta()
		{
			MenuApi::TextWrapped("%s", T("Beta features work, but have not been tested on many setups yet. All are off by default. Please report what you notice on Nexus, ideally with SPS.log.",
											"Beta-Funktionen funktionieren, sind aber noch auf wenigen Systemen erprobt. Alle sind standardmäßig aus. Auffälligkeiten bitte auf Nexus melden, am besten mit SPS.log."));
			MenuApi::SeparatorText(T("Hide occluded objects (BETA)", "Verdeckte Objekte weglassen (BETA)"));
			AtomicToggle(T("Enabled##occ", "Aktiv##occ"), Config::occlusionCull,
				T("Objects in the main view that are completely hidden behind walls, houses or terrain are not drawn. A small depth image of the previous frame decides what is hidden. Shadows, reflections and water are never affected. Pauses while the camera turns or moves fast. Saves draw calls in cities, the effect on FPS depends on your setup. Watch for objects popping in. Default OFF.",
					"Objekte im Hauptbild, die ganz hinter Mauern, Häusern oder Gelände verdeckt sind, werden nicht gezeichnet. Ein kleines Tiefenbild des vorigen Bildes entscheidet, was verdeckt ist. Schatten, Spiegelungen und Wasser sind nie betroffen. Pausiert, wenn sich die Kamera schnell dreht oder bewegt. Spart Draw Calls in Städten, wie viel FPS das bringt, hängt vom System ab. Auf aufploppende Objekte achten. Standard AUS."));
			MenuApi::TextWrapped("%s", T("Fine-tuning (only change if you know why):", "Feineinstellung (nur ändern, wenn du weißt warum):"));
			{
				const auto tune = [](const char* a_label, std::atomic<float>& a_v, float a_min, float a_max, const char* a_fmt, const char* a_tip) {
					float v = a_v.load();
					if (MenuApi::SliderFloat(a_label, &v, a_min, a_max, a_fmt)) {
						a_v.store(v);
						Config::MarkDirty();
					}
					Tip(a_tip);
				};
				const auto tuneInt = [](const char* a_label, std::atomic<std::uint32_t>& a_v, float a_min, float a_max, const char* a_fmt, const char* a_tip) {
					float v = static_cast<float>(a_v.load());
					if (MenuApi::SliderFloat(a_label, &v, a_min, a_max, a_fmt)) {
						a_v.store(static_cast<std::uint32_t>(v + 0.5f));
						Config::MarkDirty();
					}
					Tip(a_tip);
				};
				tune(T("Pause from camera turn##occ", "Pause ab Kameradrehung##occ"), Config::occlTurnDeg, 0.5f, 20.0f, "%.1f°",
					T("Hiding pauses while the camera has turned more than this since the depth image. Higher = hides more often, but objects may pop in when turning fast. Default 2.",
						"Weglassen pausiert, solange sich die Kamera seit dem Tiefenbild weiter gedreht hat. Höher = öfter weglassen, beim schnellen Drehen können aber Objekte aufploppen. Standard 2."));
				tune(T("Pause from movement##occ", "Pause ab Bewegung##occ"), Config::occlMoveUnits, 0.0f, 512.0f, "%.0f",
					T("Same for moving (game units, 64 = about 1 m). Default 64.", "Dasselbe für Bewegung (Spieleinheiten, 64 = etwa 1 m). Standard 64."));
				tune(T("Safety margin %##occ", "Sicherheitsabstand %##occ"), Config::occlMarginPct, 0.0f, 20.0f, "%.1f %%",
					T("An object only counts as hidden if it is this much farther away than what covers it. Default 2.", "Ein Objekt gilt nur als verdeckt, wenn es um so viel weiter weg ist als das, was davor liegt. Standard 2."));
				tune(T("Safety margin units##occ", "Sicherheitsabstand Einheiten##occ"), Config::occlMarginUnits, 0.0f, 256.0f, "%.0f",
					T("Plus this fixed distance. Default 16.", "Plus dieser feste Abstand. Standard 16."));
				tuneInt(T("Hidden in images in a row##occ", "Verdeckt in Bildern hintereinander##occ"), Config::occlStreak, 1.0f, 4.0f, "%.0f",
					T("How many depth images in a row an object must be hidden before it is left out. 1 = immediately. Default 2.",
						"In so vielen Tiefenbildern hintereinander muss ein Objekt verdeckt sein, bevor es weggelassen wird. 1 = sofort. Standard 2."));
				tuneInt(T("Max. size on screen (tiles)##occ", "Max. Größe im Bild (Kacheln)##occ"), Config::occlMaxTiles, 256.0f, 36864.0f, "%.0f",
					T("Larger objects are not checked (the whole image has 36864 tiles). Higher = big objects like houses can be left out too, the check costs a bit more. Default 4096.",
						"Größere Objekte werden nicht geprüft (das ganze Bild hat 36864 Kacheln). Höher = auch große Objekte wie Häuser können wegfallen, die Prüfung kostet etwas mehr. Standard 4096."));
				AtomicToggle(T("Never hide characters##occ", "Figuren nie weglassen##occ"), Config::occlSkipSkinned,
					T("Characters and other animated meshes are never left out (their bounds lag behind the animation). Default on.",
						"Figuren und andere animierte Meshes werden nie weggelassen (ihre Hülle hinkt der Animation hinterher). Standard an."));
				if (MenuApi::Button(T("Defaults##occ", "Standardwerte##occ"))) {
					Config::occlTurnDeg.store(2.0f);
					Config::occlMoveUnits.store(64.0f);
					Config::occlMarginPct.store(2.0f);
					Config::occlMarginUnits.store(16.0f);
					Config::occlStreak.store(2);
					Config::occlMaxTiles.store(4096);
					Config::occlSkipSkinned.store(true);
					Config::MarkDirty();
				}
			}
		}

		void __stdcall RenderShadows()
		{
			MenuApi::SeparatorText(T("Sun shadow culling", "Sonnenschatten kleiner Objekte weglassen"));
			Toggle(T("Enabled##sun", "Aktiv##sun"), Config::shadowCulling.enabled,
				T("Small, far objects do not cast sun shadows.", "Kleine, ferne Objekte werfen keinen Sonnenschatten."));
			RuleControls("sun", Config::shadowCulling, 5000.0f);
			float elevation = Config::sunMinElevation.load();
			if (Slider(T("Only above sun elevation (deg)", "Nur ab Sonnenhöhe (Grad)"), elevation, 0.0f, 60.0f, "%.0f",
					T("With a low sun shadows get long - below this elevation nothing is culled.", "Bei tiefer Sonne werden Schatten lang - darunter wird nichts weggelassen."))) {
				Config::sunMinElevation.store(elevation);
			}

			MenuApi::SeparatorText(T("Torch / point light shadow culling", "Fackelschatten kleiner Objekte weglassen"));
			Toggle(T("Enabled##point", "Aktiv##point"), Config::pointLightCulling.enabled,
				T("Small, far objects do not cast shadows from torches and fires.", "Kleine, ferne Objekte werfen keinen Schatten von Fackeln und Feuern."));
			AtomicToggle(T("Also in interiors##point", "Auch in Innenräumen##point"), Config::pointLightInteriors,
				T("Interiors are lit almost only by torches and fires, missing shadows are noticeable there. Off = full torch shadows indoors.",
					"Innenräume werden fast nur von Fackeln und Feuern beleuchtet, fehlende Schatten fallen dort auf. Aus = in Innenräumen volle Fackelschatten."));
			RuleControls("point", Config::pointLightCulling, 5000.0f);

			MenuApi::SeparatorText(T("Shadow cache for static lights (BETA)", "Schatten fester Lichter zwischenspeichern (BETA)"));
			Toggle(T("Enabled##lcache", "Aktiv##lcache"), Config::lightShadowCache.enabled,
				T("Shadow maps of lights that do not move (wall lamps, fireplaces) are kept: only characters and moving objects are drawn again each frame. Flickering lights are always redrawn. Mainly for interiors with many lights. BETA, default OFF: with some lighting mods (e.g. Lux) shadows near fires can pulse.",
					"Schattenkarten von Lichtern, die sich nicht bewegen (Wandleuchter, Kamine), werden aufbewahrt: Jeden Frame werden nur Figuren und Bewegliches neu gezeichnet. Flackernde Lichter werden immer neu gezeichnet. Vor allem für Innenräume mit vielen Lichtern. BETA, Standard AUS: mit manchen Lichtmods (z. B. Lux) können Schatten an Feuern pulsieren."));

			MenuApi::SeparatorText(T("Character shadow culling", "Figurenschatten in der Ferne weglassen"));
			Toggle(T("Enabled##actor", "Aktiv##actor"), Config::actorShadowCulling.enabled,
				T("Characters far away do not cast shadows.", "Weit entfernte Figuren werfen keinen Schatten."));
			Slider(T("From distance##actor", "Ab Entfernung##actor"), Config::actorShadowCulling.minDistance, Config::ActorShadowCulling::kMinAllowed, 8000.0f, "%.0f",
				T("Characters farther away than this lose their shadow (3500 = ~50 m default, 2100 = ~30 m minimum).",
					"Weiter entfernte Figuren verlieren ihren Schatten (3500 = ~50 m Standard, 2100 = ~30 m Minimum)."));
			Toggle(T("Also torch shadows##actor", "Auch Fackelschatten##actor"), Config::actorShadowCulling.pointLights,
				T("Also leave out character shadows cast by torches and fires.", "Auch Figurenschatten von Fackeln und Feuern weglassen."));

			MenuApi::SeparatorText(T("Shadow instancing (BETA)", "Gleiche Schatten bündeln (BETA)"));
			Toggle(T("Enabled##inst", "Aktiv##inst"), Config::shadowInstancing.enabled,
				T("Draws identical simple meshes in sun shadows with one draw call. Small gain, off by default - switch off again if shadows or meshes flicker.", "Zeichnet gleiche einfache Objekte im Sonnenschatten in einem Durchgang. Kleiner Gewinn, standardmäßig aus - bei flackernden Schatten oder Objekten wieder ausschalten."));
		}

		void RenderLightsAndPruning()
		{
			MenuApi::SeparatorText(T("Light assignment throttle", "Licht-Zuordnung drosseln"));
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

			MenuApi::SeparatorText(T("Subtree pruning", "Teilbäume überspringen"));
			Toggle(T("Enabled##prune", "Aktiv##prune"), Config::subtreePruning.enabled,
				T("Skips whole groups of objects in culling passes when the group as a whole already meets the culling rule.",
					"Überspringt ganze Objektgruppen, wenn schon die Gruppe als Ganzes die Regel erfüllt."));
			Toggle(T("Sun / moon shadows##prune", "Sonnen-/Mondschatten##prune"), Config::subtreePruning.sun,
				T("Apply in the sun (moon at night) shadow pass.", "Im Schattendurchlauf der Sonne (nachts Mond) anwenden."));
			Toggle(T("Rain / skylighting map##prune", "Regen-/Himmelslicht-Karte##prune"), Config::subtreePruning.precip,
				T("Apply in the precipitation / skylighting occlusion pass.", "Im Durchlauf für Regen und Himmelslicht anwenden."));

		}

		void __stdcall RenderTextures()
		{
			if (TextureStream::OtherDownscalerLoaded()) {
				MenuApi::TextWrapped("%s", T("Texture Downscaler is installed. SPS keeps the sizes it loads and only shrinks further when VRAM gets tight. If textures look wrong, switch off texture streaming here.",
												"Texture Downscaler ist installiert. SPS behält die Größen, die er lädt, und verkleinert nur weiter, wenn der VRAM knapp wird. Sehen Texturen falsch aus, hier das Textur-Streaming ausschalten."));
			}
			Toggle(T("Downscale distant textures##ts", "Ferne Texturen verkleinern##ts"), Config::textureStream.enabled,
				T("Textures of far objects (also behind you) are shrunk in VRAM and reloaded at full size from disk when you come closer. Files are never changed. Off = everything goes back to full size.",
					"Texturen ferner Objekte (auch hinter dir) werden im VRAM verkleinert und bei Annäherung in voller Größe von der Platte neu geladen. Dateien werden nie verändert. Aus = alles wieder in voller Größe."));
			{
				std::uint64_t usage = 0, budget = 0;
				TextureStream::GetVram(usage, budget);
				if (budget > 0) {
					MenuApi::Text(T("VRAM: %.1f of %.1f GB (%.0f %%)", "VRAM: %.1f von %.1f GB (%.0f %%)"), usage / 1073741824.0, budget / 1073741824.0, 100.0 * usage / budget);
				}
			}
			Toggle(T("Only when VRAM gets full##ts", "Nur wenn der VRAM knapp wird##ts"), Config::textureStream.budgetMode,
				T("Textures are only shrunk when less than the reserve below of the video memory Windows grants the game is free. With enough VRAM nothing happens and nothing is spent.",
					"Texturen werden nur verkleinert, wenn vom Grafikspeicher, den Windows dem Spiel zuteilt, weniger als der Puffer unten frei ist. Mit genug VRAM passiert nichts und es kostet nichts."));
			{
				// feste 256-MB-Stufen (Regler ueber den Index - frei gezogen landeten Zwischenwerte im Regler)
				int  step = std::clamp(static_cast<int>(std::lround(Config::textureStream.reserveMB / 256.0f)), 1, 24);
				const auto text = std::format("{} MB", step * 256);
				float      v = static_cast<float>(step);
				if (MenuApi::SliderFloat(T("Keep free##ts", "Frei halten##ts"), &v, 1.0f, 24.0f, text.c_str())) {
					step = std::clamp(static_cast<int>(std::lround(v)), 1, 24);
					Config::textureStream.reserveMB = step * 256.0f;
					Config::MarkDirty();
				}
				Tip(T("Downscaling starts when less VRAM than this is free. It absorbs sudden jumps (doors, new cells, turning around). Smaller = VRAM used more fully, larger = fewer overflows. 1024 MB fits most setups.",
					"Verkleinert wird, sobald weniger VRAM frei ist. Der Puffer fängt plötzliche Sprünge ab (Türen, neue Zellen, Umdrehen). Kleiner = VRAM voller ausgenutzt, größer = seltener Überlauf. 1024 MB passt für die meisten."));
			}
			{
				float recommend = 0.0f, jump = 0.0f;
				if (TextureStream::ReserveRecommendation(recommend, jump)) {
					MenuApi::TextWrapped(T("Recommended for your setup: %.0f MB (largest VRAM jump so far: %.1f GB within 3 s)", "Empfehlung für dein Setup: %.0f MB (größter VRAM-Sprung bisher: %.1f GB in 3 s)"),
						recommend, jump / 1024.0f);
				} else {
					MenuApi::TextWrapped("%s", T("Recommendation after a few minutes of play (SPS measures how much the VRAM jumps when you enter areas or turn around).",
													"Empfehlung nach ein paar Minuten Spielzeit (SPS misst, wie stark der VRAM beim Betreten von Gebieten oder Umdrehen springt)."));
				}
			}
			Toggle(T("Refill VRAM when there is room##ts", "VRAM wieder auffüllen, wenn Platz ist##ts"), Config::textureStream.refill,
				T("When clearly more than the reserve is free, downscaled textures are reloaded at full size again (most needed first).",
					"Ist deutlich mehr als der Puffer frei, werden verkleinerte Texturen wieder voll geladen (die meistgebrauchten zuerst)."));
			{
				int  step = std::clamp(static_cast<int>(std::lround(Config::textureStream.refillGapMB / 256.0f)), 1, 16);
				const auto text = std::format("{} MB", step * 256);
				float      v = static_cast<float>(step);
				if (MenuApi::SliderFloat(T("Gap until refilling##ts", "Abstand bis zum Wiederauffüllen##ts"), &v, 1.0f, 16.0f, text.c_str())) {
					step = std::clamp(static_cast<int>(std::lround(v)), 1, 16);
					Config::textureStream.refillGapMB = step * 256.0f;
					Config::MarkDirty();
				}
				Tip(T("Added on top of 'Keep free'. Downscaled textures are only reloaded at full size when that much is free. The gap keeps textures from going back and forth. Most users do not need to change it.",
					"Kommt zu 'Frei halten' hinzu. Erst wenn so viel frei ist, werden verkleinerte Texturen wieder voll geladen. Der Abstand verhindert, dass Texturen hin und her wechseln. Die meisten müssen ihn nicht ändern."));
				// beide Schwellen im Klartext (Regler 2 ist ein Abstand, kein eigener Grenzwert - verwirrte im Test)
				const auto keep = static_cast<int>(Config::textureStream.reserveMB), refillAt = keep + static_cast<int>(Config::textureStream.refillGapMB);
				MenuApi::TextWrapped("%s", (g_german ? std::format("Verkleinern, wenn weniger als {} MB frei sind. Wieder voll laden, wenn mehr als {} MB frei sind.", keep, refillAt) :
				                                       std::format("Downscale when less than {} MB is free. Reload at full size when more than {} MB is free.", keep, refillAt)).c_str());
				MenuApi::TextWrapped("%s", T("Most users only need 'Keep free'. Leave the gap at its default.", "Die meisten brauchen nur 'Frei halten'. Den Abstand am besten auf Standard lassen."));
			}
			Slider(T("RAM buffer (MB)##ts", "RAM-Puffer (MB)##ts"), Config::textureStream.ramCacheMB, 0.0f, 8192.0f, "%.0f",
				T("Texture data that was reloaded stays in RAM up to this size, so the next reload needs no disk access. Least recently used is dropped first. 0 = off.",
					"Neu geladene Texturdaten bleiben bis zu dieser Größe im RAM, das nächste Neuladen braucht dann keinen Plattenzugriff. Am längsten nicht gebrauchte fliegen zuerst raus. 0 = aus."));
			Toggle(T("Load at remembered size##ts", "Gleich in gemerkter Größe laden##ts"), Config::textureStream.loadReduced,
				T("The size a texture needed last time (also in earlier sessions) is used directly when the game loads it again. Less VRAM peak and loading when entering areas.",
					"Die zuletzt benötigte Größe einer Textur (auch aus früheren Sitzungen) wird direkt beim Laden verwendet. Weniger VRAM-Spitze und Laden beim Betreten von Gebieten."));
			Slider(T("Safety factor##ts", "Sicherheitsfaktor##ts"), Config::textureStream.safetyFactor, 1.0f, 4.0f, "%.1f",
				T("Needed texture size = size on screen x this factor. Higher = sharper, less saving.",
					"Benötigte Texturgröße = Größe auf dem Bildschirm x Faktor. Höher = schärfer, weniger Ersparnis."));
			{
				// Untergrenze in festen Stufen 256 / 512 / 1K / 2K / 4K (Regler ueber den Index, Anzeige als Text)
				static constexpr float kMins[] = { 256.0f, 512.0f, 1024.0f, 2048.0f, 4096.0f };
				const char*            names[] = { "256", "512", "1K", "2K", "4K" };
				int                    idx = 0;
				for (int i = 0; i < 5; ++i) {
					if (Config::textureStream.minEdge >= kMins[i] * 0.75f) {
						idx = i;
					}
				}
				float v = static_cast<float>(idx);
				if (MenuApi::SliderFloat(T("Min. size##ts", "Min. Größe##ts"), &v, 0.0f, 4.0f, names[idx])) {
					Config::textureStream.minEdge = kMins[std::clamp(static_cast<int>(std::lround(v)), 0, 4)];
					Config::MarkDirty();
				}
				Tip(T("Textures are never shrunk below this edge length. Higher = safer, less saving.",
					"Texturen werden nie unter diese Kantenlänge verkleinert. Höher = sicherer, weniger Ersparnis."));
			}
			{
				// Obergrenze: 1K / 2K / 4K / 8K / keine (Regler ueber den Index, Anzeige als Text)
				static constexpr float kCaps[] = { 1024.0f, 2048.0f, 4096.0f, 8192.0f, 0.0f };
				const char*            names[] = { "1K", "2K", "4K", "8K", T("unlimited", "unbegrenzt") };
				int                    idx = 4;
				for (int i = 0; i < 4; ++i) {
					if (Config::textureStream.maxEdge == kCaps[i]) {
						idx = i;
					}
				}
				float v = static_cast<float>(idx);
				if (MenuApi::SliderFloat(T("Max. texture size##ts", "Max. Texturgröße##ts"), &v, 0.0f, 4.0f, names[idx])) {
					Config::textureStream.maxEdge = kCaps[std::clamp(static_cast<int>(std::lround(v)), 0, 4)];
					Config::MarkDirty();
				}
				Tip(T("Upper limit for all streamed textures, near ones too, independent of VRAM usage. Applied right away and when textures load. Excluded paths and the character switches below still apply.",
					"Obergrenze für alle gestreamten Texturen, auch nahe, unabhängig von der VRAM-Belegung. Wirkt sofort und beim Laden. Ausgenommene Pfade und die Figuren-Schalter unten gelten weiter."));
			}
			Toggle(T("Also clothing and armor of characters (BETA)##ts", "Auch Kleidung und Rüstung von Figuren (BETA)##ts"), Config::textureStream.streamClothing,
				T("Clothing and armor worn by NPCs far away are shrunk as well. Distance is taken from the character itself, so sitting or animated NPCs are measured correctly. Off by default. If armor or clothing looks wrong (black, purple, blurry), turn this off.",
					"Kleidung und Rüstung weit entfernter NPCs werden ebenfalls verkleinert. Der Abstand wird an der Figur selbst gemessen, damit auch sitzende oder animierte NPCs richtig erfasst werden. Standard AUS. Sehen Rüstung oder Kleidung falsch aus (schwarz, lila, unscharf), bitte ausschalten."));
			Toggle(T("Also bodies, faces and hair (BETA)##ts", "Auch Körper, Gesichter und Haare (BETA)##ts"), Config::textureStream.streamCharacters,
				T("Body, face and hair textures (textures/actors/character) of far characters are shrunk as well. If faces or hair look wrong (black, purple, blurry), turn this off.",
					"Körper-, Gesichts- und Haartexturen (textures/actors/character) ferner Figuren werden ebenfalls verkleinert. Sehen Gesichter oder Haare falsch aus (schwarz, lila, unscharf), bitte ausschalten."));

			MenuApi::SeparatorText(T("Troubleshooting and tests (not saved)", "Fehlersuche und Tests (nicht gespeichert)"));
			if (MenuApi::Button(T("Log textures under crosshair##dbg", "Texturen unter dem Fadenkreuz protokollieren##dbg"))) {
				TextureStream::RequestCenterProbe();
			}
			Tip(T("Writes the textures of the objects in the middle of the screen (path, original and current size) to SPS.log.",
				"Schreibt die Texturen der Objekte in der Bildmitte (Pfad, Original- und aktuelle Größe) in SPS.log."));

			MenuApi::SeparatorText(T("Also include excluded types (test)", "Ausgenommene Arten mit einbeziehen (Test)"));
			MenuApi::TextWrapped("%s", T("These texture types are normally never downscaled. Tick one to see live how it behaves, best together with a low max. texture size (1K). Untick and they go back to full size.",
											"Diese Texturarten werden normalerweise nie verkleinert. Häkchen setzen, um live zu sehen, wie sie sich verhalten, am besten zusammen mit einer kleinen max. Texturgröße (1K). Häkchen weg, dann kommen sie wieder in voller Größe."));
			DebugInclude(T("Distant terrain and LOD##dbg", "Ferne Landschaft und LOD##dbg"), TextureStream::kIncludeLod,
				T("Paths with \\lod\\ or terrain\\. Distant ground, mountains and simplified objects far away.",
					"Pfade mit \\lod\\ oder terrain\\. Ferner Boden, Berge und vereinfachte Objekte in der Ferne."));
			DebugInclude(T("Effects##dbg", "Effekte##dbg"), TextureStream::kIncludeEffects,
				T("textures\\effects\\. Fire, magic, glow, smoke. Effects drawn with the effect shader are not covered at all.",
					"textures\\effects\\. Feuer, Magie, Leuchten, Rauch. Effekte mit eigenem Effekt-Shader werden gar nicht erfasst."));
			DebugInclude(T("Books##dbg", "Bücher##dbg"), TextureStream::kIncludeBooks,
				T("Every path containing 'book', also bookshelves. Excluded so text stays readable.",
					"Alle Pfade mit 'book', auch Bücherregale. Ausgenommen, damit Schrift lesbar bleibt."));
			DebugInclude(T("Sky##dbg", "Himmel##dbg"), TextureStream::kIncludeSky,
				T("Paths with \\sky\\. Clouds, moons, stars - most use their own shader and are not covered.",
					"Pfade mit \\sky\\. Wolken, Monde, Sterne - die meisten haben einen eigenen Shader und werden nicht erfasst."));
			DebugInclude(T("Reflections (cubemaps)##dbg", "Reflexionen (Cubemaps)##dbg"), TextureStream::kIncludeCubemaps,
				T("cubemaps\\. Real cube maps can never be downscaled, only flat textures in this folder.",
					"cubemaps\\. Echte Würfelkarten lassen sich nie verkleinern, nur flache Texturen in diesem Ordner."));
			MenuApi::TextWrapped("%s", T("Not possible at all: grass, water and effect shader textures (other shaders), textures without mipmaps.",
											"Gar nicht möglich: Gras, Wasser und Effekt-Shader-Texturen (andere Shader), Texturen ohne Mipmaps."));
		}

		void __stdcall RenderScene()
		{
			RenderLightsAndPruning();

			MenuApi::SeparatorText(T("Skylighting culling (Community Shaders)", "Himmelslicht: kleine Objekte weglassen (Community Shaders)"));
			Toggle(T("Enabled##sky", "Aktiv##sky"), Config::skylightingCulling.enabled,
				T("Small objects are left out of the skylighting occlusion map.", "Kleine Objekte werden in der Himmelslicht-Karte weggelassen."));
			Slider(T("Min. object radius##sky", "Min. Objektradius##sky"), Config::skylightingCulling.minRadius, 32.0f, 512.0f, "%.0f",
				T("Objects smaller than this are left out.", "Kleinere Objekte werden weggelassen."));

			MenuApi::SeparatorText(T("Decal culling", "Ferne Bodendetails weglassen"));
			Toggle(T("Enabled##decal", "Aktiv##decal"), Config::decalCulling.enabled,
				T("Small decals far away are not drawn.", "Kleine Bodendetails in der Ferne werden nicht gezeichnet."));
			Slider(T("From distance##decal", "Ab Entfernung##decal"), Config::decalCulling.maxDistance, 500.0f, 5000.0f, "%.0f",
				T("Decals farther away than this are not drawn. Below 1500 leaves can disappear.",
					"Weiter entfernte Decals werden nicht gezeichnet. Unter 1500 können Blätter auf dem Boden verschwinden."));
			Slider(T("Max. decal radius##decal", "Max. Radius##decal"), Config::decalCulling.maxRadius, 10.0f, 500.0f, "%.0f",
				T("Only decals smaller than this are affected. Footprints are about 22. From about 50 plaster and stone patches on walls disappear too (visible).",
					"Nur kleinere Bodendetails sind betroffen. Fußabdrücke haben etwa 22. Ab etwa 50 verschwinden auch Putz- und Steinflecken an Mauern (sichtbar)."));
		}
		// Hilfe: Bildfehler -> welcher Schalter
		void HelpEntry(const char* a_symptom, const char* a_switch)
		{
			MenuApi::BulletText("%s", a_symptom);
			MenuApi::TextWrapped("      -> %s", a_switch);
		}

		void __stdcall RenderHelp()
		{
			MenuApi::TextWrapped("%s", T("Something looks wrong? First press the hotkey (Page Up by default) to switch all optimizations off. If the issue stays, it does not come from SPS. If it goes away, switch on again and turn off the switch listed below for your issue.",
											"Sieht etwas falsch aus? Zuerst mit der Taste (Standard Bild auf) alle Optimierungen ausschalten. Bleibt der Fehler, kommt er nicht von SPS. Verschwindet er, wieder einschalten und den unten genannten Schalter für den Fehler ausschalten."));

			MenuApi::SeparatorText(T("Shadows", "Schatten"));
			HelpEntry(T("Small objects in the distance have no sun shadow", "Kleine Objekte in der Ferne haben keinen Sonnenschatten"),
				T("Shadows > Sun shadow culling (or lower 'Min. apparent size')", "Schatten > Sonnenschatten kleiner Objekte weglassen (oder 'Min. scheinbare Größe' senken)"));
			HelpEntry(T("Objects near torches or fires are missing their shadow", "Objekten an Fackeln oder Feuern fehlt der Schatten"),
				T("Shadows > Torch / point light shadow culling (in interiors: 'Also in interiors')", "Schatten > Fackelschatten kleiner Objekte weglassen (in Innenräumen: 'Auch in Innenräumen')"));
			HelpEntry(T("Characters far away have no shadow", "Figuren in der Ferne haben keinen Schatten"),
				T("Shadows > Character shadow culling (or raise the distance)", "Schatten > Figurenschatten in der Ferne weglassen (oder Entfernung erhöhen)"));
			HelpEntry(T("A torch light briefly goes dark, or a shadow stays where a door or object used to be", "Ein Fackellicht wird kurz dunkel, oder ein Schatten bleibt, wo eine Tür oder ein Objekt war"),
				T("Shadows > Shadow cache for static lights", "Schatten > Schatten fester Lichter zwischenspeichern"));
			HelpEntry(T("Sun shadows or objects flicker", "Sonnenschatten oder Objekte flackern"),
				T("Shadows > Shadow instancing", "Schatten > Gleiche Schatten bündeln"));
			HelpEntry(T("Far shadows follow the sun in small steps", "Ferne Schatten folgen der Sonne in kleinen Schritten"),
				T("Overview > Calmer far shadows", "Übersicht > Ruhigere ferne Schatten"));

			MenuApi::SeparatorText(T("Textures", "Texturen"));
			HelpEntry(T("Textures are blurry up close or get sharp only after a moment", "Texturen sind aus der Nähe unscharf oder werden erst nach einem Moment scharf"),
				T("Texture streaming > Downscale distant textures (or raise 'Safety factor' / 'Min. size')", "Textur-Streaming > Ferne Texturen verkleinern (oder 'Sicherheitsfaktor' / 'Min. Größe' erhöhen)"));
			HelpEntry(T("Faces or hair look black, purple or blurry", "Gesichter oder Haare sehen schwarz, lila oder unscharf aus"),
				T("Texture streaming > Also bodies, faces and hair", "Textur-Streaming > Auch Körper, Gesichter und Haare"));
			if (TextureStream::OtherDownscalerLoaded()) {
				HelpEntry(T("Texture errors with Texture Downscaler installed", "Texturfehler mit installiertem Texture Downscaler"),
					T("Texture streaming > Downscale distant textures off (Texture Downscaler then handles VRAM alone)", "Textur-Streaming > Ferne Texturen verkleinern aus (Texture Downscaler regelt dann den VRAM allein)"));
			}
			HelpEntry(T("Clothing or armor of NPCs looks blurry", "Kleidung oder Rüstung von NPCs ist unscharf"),
				T("Texture streaming > Also clothing and armor of characters", "Textur-Streaming > Auch Kleidung und Rüstung von Figuren"));
			HelpEntry(T("Use 'Log textures under crosshair' to report a texture issue (writes path and size to SPS.log)", "Für Texturfehler 'Texturen unter dem Fadenkreuz protokollieren' nutzen (schreibt Pfad und Größe in SPS.log)"),
				T("Texture streaming > Log textures under crosshair", "Textur-Streaming > Texturen unter dem Fadenkreuz protokollieren"));

			MenuApi::SeparatorText(T("Scene", "Szene"));
			HelpEntry(T("Footprints, blood or leaves on the ground are missing in the distance", "Fußspuren, Blut oder Laub auf dem Boden fehlen in der Ferne"),
				T("Lights and scene > Decal culling", "Licht und Szene > Ferne Bodendetails weglassen"));
			HelpEntry(T("Rain or snow falls through small roofs, or small objects look wrong in skylighting (Community Shaders)", "Regen oder Schnee fällt durch kleine Dächer, oder kleine Objekte wirken im Himmelslicht falsch (Community Shaders)"),
				T("Lights and scene > Skylighting culling", "Licht und Szene > Himmelslicht: kleine Objekte weglassen"));
			HelpEntry(T("Characters walking past a torch are lit a moment late", "An einer Fackel vorbeilaufende Figuren werden einen Moment zu spät beleuchtet"),
				T("Lights and scene > Light assignment throttle (or lower 'Max. age')", "Licht und Szene > Licht-Zuordnung drosseln (oder 'Max. Alter' senken)"));
			HelpEntry(T("Whole objects disappear, or tiny objects pop in", "Ganze Objekte verschwinden, oder winzige Objekte ploppen auf"),
				T("Overview > Beta: Main view micro culling or Hide occluded objects (off by default)", "Übersicht > Beta: Winzige Objekte im Bild weglassen oder Verdeckte Objekte weglassen (standardmäßig aus)"));

			MenuApi::SeparatorText(T("Reporting a bug", "Fehler melden"));
			MenuApi::TextWrapped("%s", T("Please attach SPS.log (Documents\\My Games\\Skyrim Special Edition\\SKSE) and say where it happened and which switch makes it go away.",
											"Bitte SPS.log anhängen (Dokumente\\My Games\\Skyrim Special Edition\\SKSE) und sagen, wo es passiert ist und welcher Schalter es behebt."));
		}
	}

	bool IsGerman()
	{
		static const bool german = DetectGerman();
		return german;
	}

	void Register()
	{
		if (!MenuApi::IsInstalled()) {
			logger::warn("SKSE Menu Framework not installed - no menu, settings via INI only");
			return;
		}
		g_german = IsGerman();
		MenuApi::AddSectionItem(kSection + T("Overview", "Übersicht"), RenderOverview);
		MenuApi::AddSectionItem(kSection + T("Shadows", "Schatten"), RenderShadows);
		MenuApi::AddSectionItem(kSection + T("Texture streaming", "Textur-Streaming"), RenderTextures);
		MenuApi::AddSectionItem(kSection + T("Lights and scene", "Licht und Szene"), RenderScene);
		MenuApi::AddSectionItem(kSection + T("Help with visual issues", "Hilfe bei Bildfehlern"), RenderHelp);
		MenuApi::AddSectionItem(kSection + "Beta", RenderBeta);
		MenuApi::AddSectionItem(kSection + "Debug", RenderDebug);
		logger::info("Menu registered in SKSE Menu Framework (version {:.1f})", MenuApi::Version());
	}
}
