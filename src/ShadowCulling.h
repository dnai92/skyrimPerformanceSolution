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
	// Accumulator der Hauptansicht (Weltkamera) - nicht Spiegelungen, Vorschauen, Menue-3D
	bool IsMainViewAccumulator(const void* a_accumulator) noexcept;

	// Cache der fernen Kaskade: vor bzw. nach BSShadowDirectionalLight::Accumulate (Main-Thread)
	void BeforeSunAccumulate(RE::BSShadowDirectionalLight* a_light) noexcept;
	void AfterSunAccumulate(RE::BSShadowDirectionalLight* a_light) noexcept;
	// Nach BSShadowDirectionalLight::Render: Ergebnis des Caches der fernen Kaskade verbuchen
	void AfterSunRender() noexcept;
	// Cache der fernen Kaskade am Zeichnen einer Schattenkarte (Hook ID 107604+0x167, Render-Thread):
	// true = diese Karte ist die ferne Sonnenkaskade und wird gefiltert; danach FarEndDraw aufrufen
	bool FarBeginDraw(const RE::NiCamera* a_camera) noexcept;
	void FarEndDraw() noexcept;
	// Einzelner Draw waehrend der fernen Kaskade: true = weglassen
	bool FarSkipDraw(const RE::BSRenderPass& a_pass) noexcept;
	// Zeichnen einer Schattenkarte (Sonnenkaskade, Fackel/Lampe) laeuft - gesetzt vom Hook ID 107604+0x167
	void SetInShadowMapDraw(bool a_in) noexcept;
	// Einzelner Schatten-Draw: kleines Figurenteil in der Ferne -> true = weglassen (ActorPartShadowCulling)
	bool SkipActorPartShadowDraw(const RE::BSRenderPass& a_pass) noexcept;
	// Ladebildschirm / Hauptmenue: Zustand der Welt (Cache der fernen Kaskade, Hysterese der Figurenschatten) verwerfen
	void ResetWorld() noexcept;
	// Nach BSShadowDirectionalLight::UpdateCamera: ferne Kaskade ruhig halten, Modus des Caches festlegen
	void AfterSunUpdateCamera(RE::BSShadowDirectionalLight* a_light) noexcept;
	// Diagnose: 30 Frames lang alle im Hauptbild weggelassenen Objekte (Mikro-Culling, ferne Decals) sammeln und als
	// SPS_MicroCulled_N.csv in den SKSE-Log-Ordner schreiben
	void RequestMicroCullList() noexcept;

	// Einmal pro Frame auf dem Main-Thread: Kamera-Position und Kaskaden-Culler der Sonne cachen
	void OnFrame();
}
