#pragma once

// Messung Tiefenvorpass (Debug-Knopf): Was kostet Main::RenderDepth auf der GPU, und welche Objektarten tragen dazu bei?
// Wechselt alle 8 s, welche Art im Vorpass weggelassen wird (keine, Gras, Alpha-Test, Baeume, Landschaft, kleine
// ferne Objekte, Figuren), und schreibt je Phase GPU-/CPU-Zeit des Vorpasses und die Draws je Art in SPS.log
// ([Prepass-AB]). Waehrend der Messung kann das Bild kurz fehlerhaft sein (Weglassen nur zum Messen).
namespace PrepassAnalysis
{
	void Start() noexcept;
	bool Running() noexcept;

	// Einmal pro Frame (Main-Thread)
	void OnFrame() noexcept;

	// Einzelner Draw im Vorpass (Main-Thread): zaehlen; true = in dieser Phase weglassen
	bool SkipDraw(const RE::BSRenderPass& a_pass, bool a_alphaTest) noexcept;

	// CPU-Zeit eines Vorpass-Aufrufs
	void AddCpuNs(std::int64_t a_ns) noexcept;
}
