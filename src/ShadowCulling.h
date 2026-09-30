#pragma once

// Kleinobjekt-Culling fuer die Sonnenschatten.
// Hook auf BSCullingProcess::AppendVirtual (vfunc 0x18): Wird ein Mesh in die Liste einer Sonnen-Kaskade
// aufgenommen, das weit weg und im Verhaeltnis zur Entfernung klein ist, wird es verworfen.
// Zaehlt ausserdem, wie viele Meshes pro Kaskade aufgenommen werden (auch bei deaktiviertem Culling).
namespace ShadowCulling
{
	void Install();

	// Hooks, die NACH Community Shaders installiert werden muessen (kDataLoaded)
	void InstallLate();

	// Tiefenvorpass: true, wenn der Draw dieses Passes uebersprungen werden soll (nur waehrend Main::RenderDepth)
	bool ShouldSkipDepthPrepassDraw(const RE::BSRenderPass& a_pass) noexcept;
	bool InDepthPrepass() noexcept;

	// Kaskaden-Cache: vor bzw. nach BSShadowDirectionalLight::Accumulate (Main-Thread)
	void BeforeSunAccumulate(RE::BSShadowDirectionalLight* a_light) noexcept;
	void AfterSunAccumulate(RE::BSShadowDirectionalLight* a_light) noexcept;
	// Nach BSShadowDirectionalLight::Render: ferne Kaskade sichern (gezeichneter Frame) bzw. zuruecklegen (Cache-Frame)
	void AfterSunRender() noexcept;

	// Einmal pro Frame auf dem Main-Thread: Kamera-Position und Kaskaden-Culler der Sonne cachen
	void OnFrame();
}
