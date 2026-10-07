#include "Menu.h"

#include "Config.h"
#include "Features.h"
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
			const auto label = g_capturingKey ? std::string(T("Press a key... (Esc = cancel)", "Taste drücken ... (Esc = abbrechen)")) : KeyName(current);
			if (MenuApi::Button((label + "###hotkey").c_str())) {
				g_capturingKey = true;
			}
			MenuApi::SameLine();
			MenuApi::TextUnformatted(T("Hotkey for the master switch", "Taste für den Hauptschalter"));
			Tip(T("Click, then press the new key. Works immediately and is saved.", "Anklicken, dann die neue Taste drücken. Wirkt sofort und wird gespeichert."));
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
			MenuApi::TextWrapped("%s", T("Use this switch (or the hotkey) to compare FPS and look with and without SPS. The 10-second report in SPS.log shows the numbers.",
											"Mit diesem Schalter (oder der Taste) FPS und Bild mit und ohne SPS vergleichen. Der 10-Sekunden-Bericht in SPS.log zeigt die Zahlen."));

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
			Toggle(T("Shadow instancing", "Gleiche Schatten bündeln"), Config::shadowInstancing.enabled,
				T("Draws identical simple meshes in sun shadows with one draw call. Small gain, off by default - switch off again if shadows or meshes flicker.", "Zeichnet gleiche einfache Objekte im Sonnenschatten in einem Durchgang. Kleiner Gewinn, standardmäßig aus - bei flackernden Schatten oder Objekten wieder ausschalten."));
			Toggle(T("Light assignment throttle", "Licht-Zuordnung drosseln"), Config::lightGather.enabled,
				T("Moving lights (torches, flickering lights) only search for the objects they light when they really moved.",
					"Bewegte Lichter (Fackeln, flackernde Lichter) suchen die beleuchteten Objekte nur neu, wenn sie sich wirklich bewegt haben."));
			Toggle(T("Subtree pruning", "Teilbäume überspringen"), Config::subtreePruning.enabled,
				T("Skips whole groups of objects in the shadow and skylighting passes when the group as a whole is already small and far enough to be culled. Same result, less work.",
					"Überspringt ganze Objektgruppen im Schatten- und Himmelslicht-Durchlauf, wenn schon die Gruppe als Ganzes klein und weit genug ist. Gleiches Ergebnis, weniger Arbeit."));
			Toggle(T("Texture streaming", "Textur-Streaming"), Config::textureStream.enabled,
				T("Textures of far objects are shrunk in VRAM and reloaded at full size when you come closer.",
					"Texturen ferner Objekte werden im VRAM verkleinert und bei Annäherung in voller Größe neu geladen."));

			MenuApi::SeparatorText(T("Experimental (known side effects)", "Experimentell (bekannte Nebenwirkungen)"));
			Toggle(T("Depth pre-pass culling", "Tiefenvorpass: kleine Objekte weglassen"), Config::depthPrepassCulling.enabled,
				T("Can make whole objects disappear. Default OFF.", "Kann ganze Objekte verschwinden lassen. Standard AUS."));
			Toggle(T("Main view micro culling", "Winzige Objekte im Bild weglassen"), Config::mainViewCulling.enabled,
				T("Tiny far objects are not drawn at all - can pop in. Default OFF.", "Winzige ferne Objekte werden gar nicht gezeichnet - können aufploppen. Standard AUS."));
			Toggle(T("Far shadow cascade cache", "Cache ferne Schattenkaskade"), Config::cascadeCache.enabled,
				T("Far sun shadows only every 2nd frame - flickers with a low sun. Default OFF.", "Ferne Sonnenschatten nur jeden 2. Frame - flackert bei tiefer Sonne. Standard AUS."));

			MenuApi::SeparatorText(T("Settings", "Einstellungen"));
			if (MenuApi::Button(T("Reset to defaults", "Auf Standard zurücksetzen"))) {
				Config::RequestReset();
			}
			Tip(T("Deletes SPS_User.ini and reloads the defaults from SPS.ini (takes up to 2 s).",
				"Löscht SPS_User.ini und lädt die Standardwerte aus SPS.ini (dauert bis zu 2 s)."));
			AtomicToggle(T("Analysis logging (costs performance)", "Analyse-Protokoll (kostet Leistung)"), Config::analysis,
				T("Extra diagnostics in SPS.log (incl. detailed texture streaming report every 10 s). Costs 2-4 ms per frame - only for troubleshooting.",
					"Zusätzliche Diagnose in SPS.log (inkl. ausführlichem Textur-Streaming-Bericht alle 10 s). Kostet 2-4 ms pro Frame - nur zur Fehlersuche."));
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

			MenuApi::SeparatorText(T("Character shadow culling", "Figurenschatten in der Ferne weglassen"));
			Toggle(T("Enabled##actor", "Aktiv##actor"), Config::actorShadowCulling.enabled,
				T("Characters far away do not cast shadows.", "Weit entfernte Figuren werfen keinen Schatten."));
			Slider(T("From distance##actor", "Ab Entfernung##actor"), Config::actorShadowCulling.minDistance, Config::ActorShadowCulling::kMinAllowed, 8000.0f, "%.0f",
				T("Characters farther away than this lose their shadow (3500 = ~50 m default, 2100 = ~30 m minimum).",
					"Weiter entfernte Figuren verlieren ihren Schatten (3500 = ~50 m Standard, 2100 = ~30 m Minimum)."));
			Toggle(T("Also torch shadows##actor", "Auch Fackelschatten##actor"), Config::actorShadowCulling.pointLights,
				T("Also leave out character shadows cast by torches and fires.", "Auch Figurenschatten von Fackeln und Feuern weglassen."));

			MenuApi::SeparatorText(T("Shadow instancing", "Gleiche Schatten bündeln"));
			Toggle(T("Enabled##inst", "Aktiv##inst"), Config::shadowInstancing.enabled,
				T("Draws identical simple meshes in sun shadows with one draw call. Small gain, off by default - switch off again if shadows or meshes flicker.", "Zeichnet gleiche einfache Objekte im Sonnenschatten in einem Durchgang. Kleiner Gewinn, standardmäßig aus - bei flackernden Schatten oder Objekten wieder ausschalten."));
		}

		void __stdcall RenderScene()
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

			MenuApi::SeparatorText(T("Texture streaming", "Textur-Streaming"));
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
				T("Textures are only shrunk when the game uses more than the threshold below of the video memory Windows grants it. With enough VRAM nothing happens and nothing is spent.",
					"Texturen werden nur verkleinert, wenn das Spiel mehr als die Schwelle unten vom Grafikspeicher belegt, den Windows ihm zuteilt. Mit genug VRAM passiert nichts und es kostet nichts."));
			Slider(T("Start at (%)##ts", "Ab Belegung (%)##ts"), Config::textureStream.budgetStartPct, 50.0f, 100.0f, "%.0f",
				T("VRAM usage (percent of the budget) at which downscaling starts.", "VRAM-Belegung (Prozent des Budgets), ab der verkleinert wird."));
			Toggle(T("Refill VRAM when there is room##ts", "VRAM wieder auffüllen, wenn Platz ist##ts"), Config::textureStream.refill,
				T("When usage drops clearly below the threshold, downscaled textures are reloaded at full size again (most needed first).",
					"Fällt die Belegung deutlich unter die Schwelle, werden verkleinerte Texturen wieder voll geladen (die meistgebrauchten zuerst)."));
			Slider(T("Refill below threshold minus (%)##ts", "Auffüllen ab Schwelle minus (%)##ts"), Config::textureStream.refillGapPct, 2.0f, 30.0f, "%.0f",
				T("Gap between downscaling and refilling, so textures do not go back and forth.", "Abstand zwischen Verkleinern und Auffüllen, damit Texturen nicht hin und her wechseln."));
			Slider(T("RAM buffer (MB)##ts", "RAM-Puffer (MB)##ts"), Config::textureStream.ramCacheMB, 0.0f, 8192.0f, "%.0f",
				T("Texture data that was reloaded stays in RAM up to this size, so the next reload needs no disk access. Least recently used is dropped first. 0 = off.",
					"Neu geladene Texturdaten bleiben bis zu dieser Größe im RAM, das nächste Neuladen braucht dann keinen Plattenzugriff. Am längsten nicht gebrauchte fliegen zuerst raus. 0 = aus."));
			Toggle(T("Load at remembered size##ts", "Gleich in gemerkter Größe laden##ts"), Config::textureStream.loadReduced,
				T("The size a texture needed last time (also in earlier sessions) is used directly when the game loads it again. Less VRAM peak and loading when entering areas.",
					"Die zuletzt benötigte Größe einer Textur (auch aus früheren Sitzungen) wird direkt beim Laden verwendet. Weniger VRAM-Spitze und Laden beim Betreten von Gebieten."));
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
			if (MenuApi::Button(T("Log textures under crosshair##ts", "Texturen unter dem Fadenkreuz protokollieren##ts"))) {
				TextureStream::RequestCenterProbe();
			}
			Tip(T("Writes the textures of the objects in the middle of the screen (path, original and current size) to SPS.log. For bug reports.",
				"Schreibt die Texturen der Objekte in der Bildmitte (Pfad, Original- und aktuelle Größe) in SPS.log. Für Fehlerberichte."));

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
		MenuApi::AddSectionItem(kSection + T("Lights and scene", "Licht und Szene"), RenderScene);
		logger::info("Menu registered in SKSE Menu Framework (version {:.1f})", MenuApi::Version());
	}
}
