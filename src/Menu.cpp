#include "Menu.h"

#include "Config.h"

#include "third_party/SKSEMenuFramework.h"

namespace Menu
{
	namespace
	{
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
			Slider("Min. distance", a_rule.minDistance, 0.0f, a_maxDistance, "%.0f", "Objects closer to the camera always keep their shadow (game units, 70 = 1 m).");
			Slider("Max. object radius", a_rule.maxRadius, 10.0f, 500.0f, "%.0f", "Bigger objects always keep their shadow.");
			Slider("Min. apparent size", a_rule.minAngularSize, 0.001f, 0.1f, "%.3f", "Radius / distance. Objects that appear smaller than this lose their shadow. Higher = more culling.");
			ImGuiMCP::PopID();
		}

		void __stdcall RenderOverview()
		{
			ImGuiMCP::SeparatorText("Master switch");
			AtomicToggle("All optimizations active", Config::masterEnabled,
				"Turns every optimization on/off at once (same as the hotkey, Page Up by default). Not saved - starts ON.", false);
			ImGuiMCP::TextWrapped("Use this switch (or the hotkey) to compare FPS and look with and without SkyrimPerf. The 10-second report in SkyrimPerf.log shows the numbers.");

			ImGuiMCP::SeparatorText("Optimizations");
			Toggle("Sun shadow culling", Config::shadowCulling.enabled, "Small, far objects do not cast sun shadows.");
			Toggle("Torch / point light shadow culling", Config::pointLightCulling.enabled, "Small, far objects do not cast shadows from torches and fires.");
			Toggle("Character shadow culling", Config::actorShadowCulling.enabled, "Characters far away do not cast shadows.");
			Toggle("Skylighting culling (Community Shaders)", Config::skylightingCulling.enabled, "Small objects are left out of the skylighting occlusion map.");
			Toggle("Decal culling", Config::decalCulling.enabled, "Small decals (footprints, blood, dirt) far away are not drawn.");
			Toggle("Shadow instancing", Config::shadowInstancing.enabled, "Draws identical simple meshes in sun shadows with one draw call.");
			Toggle("Light assignment throttle", Config::lightGather.enabled, "Moving lights (torches, flickering lights) only search for the objects they light when they really moved.");
			Toggle("Subtree pruning", Config::subtreePruning.enabled, "Skips whole groups of objects in the shadow and skylighting passes when the group as a whole is already small and far enough to be culled. Same result, less work.");

			ImGuiMCP::SeparatorText("Experimental (known side effects)");
			Toggle("Depth pre-pass culling", Config::depthPrepassCulling.enabled, "Can make whole objects disappear. Default OFF.");
			Toggle("Main view micro culling", Config::mainViewCulling.enabled, "Tiny far objects are not drawn at all - can pop in. Default OFF.");
			Toggle("Far shadow cascade cache", Config::cascadeCache.enabled, "Far sun shadows only every 2nd frame - flickers with a low sun. Default OFF.");

			ImGuiMCP::SeparatorText("Settings");
			if (ImGuiMCP::Button("Reset to defaults")) {
				Config::RequestReset();
			}
			Tip("Deletes SkyrimPerf_User.ini and reloads the defaults from SkyrimPerf.ini (takes up to 2 s).");
			AtomicToggle("Analysis logging (costs performance)", Config::analysis, "Extra diagnostics in SkyrimPerf.log. Costs 2-4 ms per frame - only for troubleshooting.");
		}

		void __stdcall RenderShadows()
		{
			ImGuiMCP::SeparatorText("Sun shadow culling");
			Toggle("Enabled##sun", Config::shadowCulling.enabled, "Small, far objects do not cast sun shadows.");
			RuleControls("sun", Config::shadowCulling, 5000.0f);
			float elevation = Config::sunMinElevation.load();
			if (Slider("Only above sun elevation (deg)", elevation, 0.0f, 60.0f, "%.0f", "With a low sun shadows get long - below this elevation nothing is culled.")) {
				Config::sunMinElevation.store(elevation);
			}

			ImGuiMCP::SeparatorText("Torch / point light shadow culling");
			Toggle("Enabled##point", Config::pointLightCulling.enabled, "Small, far objects do not cast shadows from torches and fires.");
			RuleControls("point", Config::pointLightCulling, 5000.0f);

			ImGuiMCP::SeparatorText("Character shadow culling");
			Toggle("Enabled##actor", Config::actorShadowCulling.enabled, "Characters far away do not cast shadows.");
			Slider("From distance##actor", Config::actorShadowCulling.minDistance, 500.0f, 8000.0f, "%.0f", "Characters farther away than this lose their shadow (2500 = ~35 m).");
			Toggle("Also torch shadows##actor", Config::actorShadowCulling.pointLights, "Also leave out character shadows cast by torches and fires.");

			ImGuiMCP::SeparatorText("Shadow instancing");
			Toggle("Enabled##inst", Config::shadowInstancing.enabled, "Draws identical simple meshes in sun shadows with one draw call.");
		}

		void __stdcall RenderScene()
		{
			ImGuiMCP::SeparatorText("Light assignment throttle");
			Toggle("Enabled##light", Config::lightGather.enabled, "Moving lights only search for the objects they light when they really moved.");
			Slider("Min. movement", Config::lightGather.minMove, 1.0f, 64.0f, "%.0f", "A light searches again when it moved at least this far (game units). Flickering moves lights only a little.");
			Slider("Min. radius change", Config::lightGather.minRadiusChange, 1.0f, 128.0f, "%.0f", "... or its radius changed by this much ...");
			Slider("Max. age (ms)", Config::lightGather.maxAgeMs, 16.0f, 1000.0f, "%.0f", "... or the last search is older than this. Lower = characters walking past get lit sooner.");

			ImGuiMCP::SeparatorText("Subtree pruning");
			Toggle("Enabled##prune", Config::subtreePruning.enabled, "Skips whole groups of objects in culling passes when the group as a whole already meets the culling rule.");
			Toggle("Sun / moon shadows##prune", Config::subtreePruning.sun, "Apply in the sun (moon at night) shadow pass.");
			Toggle("Rain / skylighting map##prune", Config::subtreePruning.precip, "Apply in the precipitation / skylighting occlusion pass.");

			ImGuiMCP::SeparatorText("Texture streaming");
			Toggle("Downscale distant textures##ts", Config::textureStream.enabled, "Textures of far objects (also behind you) are shrunk in VRAM and reloaded at full size from disk when you come closer. Files are never changed. Off = everything goes back to full size.");
			Toggle("Report in log##ts", Config::textureStream.analysis, "Every 10 s: textures managed, downscaled, VRAM saved, reloads. Results in SkyrimPerf.log.");
			Slider("Safety factor##ts", Config::textureStream.safetyFactor, 1.0f, 4.0f, "%.1f", "Needed texture size = size on screen x this factor. Higher = sharper, less saving.");
			if (Slider("Min. size (px)##ts", Config::textureStream.minEdge, 256.0f, 4096.0f, "%.0f", "Textures are never shrunk below this edge length. Higher = safer, less saving.")) {
				float p = 256.0f;
				while (p * 1.5f < Config::textureStream.minEdge) {
					p *= 2.0f;
				}
				Config::textureStream.minEdge = p;
			}

			ImGuiMCP::SeparatorText("Skylighting culling (Community Shaders)");
			Toggle("Enabled##sky", Config::skylightingCulling.enabled, "Small objects are left out of the skylighting occlusion map.");
			Slider("Min. object radius##sky", Config::skylightingCulling.minRadius, 32.0f, 512.0f, "%.0f", "Objects smaller than this are left out.");

			ImGuiMCP::SeparatorText("Decal culling");
			Toggle("Enabled##decal", Config::decalCulling.enabled, "Small decals far away are not drawn.");
			Slider("From distance##decal", Config::decalCulling.maxDistance, 500.0f, 5000.0f, "%.0f", "Decals farther away than this are not drawn. Below 1500 leaves can disappear.");
			Slider("Max. decal radius##decal", Config::decalCulling.maxRadius, 10.0f, 500.0f, "%.0f", "Only decals smaller than this are affected.");
		}
	}

	void Register()
	{
		if (!SKSEMenuFramework::IsInstalled()) {
			logger::warn("SKSE Menu Framework nicht installiert - kein Menue, Einstellungen nur ueber die INI");
			return;
		}
		SKSEMenuFramework::SetSection("SkyrimPerf");
		SKSEMenuFramework::AddSectionItem("Overview", RenderOverview);
		SKSEMenuFramework::AddSectionItem("Shadows", RenderShadows);
		SKSEMenuFramework::AddSectionItem("Lights and scene", RenderScene);
		logger::info("Menue im SKSE Menu Framework registriert (Version {:.1f})", SKSEMenuFramework::GetMenuFrameworkVersion());
	}
}
