#pragma once

// Verdeckungs-Experiment (Branch exp/occlusion), Stufe 1: nur messen, nichts weglassen.
// Nach dem Tiefenvorpass (Main::RenderDepth) wird die Kamera gemerkt, beim Present der Tiefenpuffer des Vorpasses
// auf der GPU auf 256x144 Kacheln zusammengefasst und ohne Warten zurueckgelesen. Jedes Objekt der Hauptszene
// (GetRenderPasses) wird gegen das zuletzt angekommene Bild geprueft: liegt seine naechste Stelle hinter dem fernsten
// Wert aller Kacheln, die es abdeckt, waere es verdeckt. Minutenbericht in SPS.log ([Occlusion]).
namespace Occlusion
{
	// Main::RenderDepth beginnt (Main-Thread): GPU-Matrix der Hauptkamera merken
	void OnDepthPrepassBegin() noexcept;

	// Main::RenderDepth fertig (Main-Thread): Kamera dieses Frames merken
	void OnDepthPrepassEnd() noexcept;

	// IDXGISwapChain::Present (Main-Thread): fertiges Ergebnis abholen, neuen Tiefenpuffer einreichen, Bericht
	void OnPresent() noexcept;

	// GetRenderPasses der Hauptszene: Objekt zaehlen (a_draws = Anzahl Render-Passes).
	// true = Objekt weglassen (Stufe 2: im Hauptbild zweimal hintereinander verdeckt, Kamera ruhig)
	bool CountMain(const RE::BSGeometry& a_geom, const void* a_accumulator, std::uint32_t a_draws) noexcept;

	// Weglassen als Draw-Filter: im Hauptbild verdecktes Objekt fuer dieses Bild merken (Sammel-Threads) bzw. pruefen
	// (Main-Thread beim Zeichnen). Gilt nur fuer das Bild, in dem es gemerkt wurde (Zaehler am Present).
	void MarkHidden(const RE::BSGeometry* a_geom) noexcept;
	bool IsHidden(const RE::BSGeometry* a_geom) noexcept;
	void CountFilterSkip() noexcept;

	// Ladebildschirm / Hauptmenue
	void Reset() noexcept;

	// Tuer wird aktiviert / oeffnet oder schliesst sich: fuer a_seconds als bewegtes Objekt aus dem alten Tiefenbild nehmen
	void NoteDoorMoving(RE::TESObjectREFR* a_door, float a_seconds) noexcept;

	// Kamera des Frames steht fest (nach UpdateCamera der Sonne, Main-Thread): Pyramide im Hilfsthread anstossen
	void OnViewCamera() noexcept;

	// Main::Begin im Hauptbild (vor den Sammel-Jobs, Kamera steht fest): Pyramide so frueh wie moeglich anstossen
	void OnFrameBegin() noexcept;
	void SetFrameBeginHooked() noexcept;  // Main::Begin-Hook installiert: Bilder dort zaehlen statt am Present

	// Bottled Shaders mit eigener HiZ-Verdeckung erkennen (Daten geladen, Ladebildschirm)
	void DetectOtherCullers() noexcept;
	bool BottledHiZActive() noexcept;
	bool BottledInstalled() noexcept;
	// Schatten-Cache fester Lichter wegen Bottled Shaders aus (Flackern in Innenraeumen, nicht erlaubt)
	bool BottledBlocksLightCache() noexcept;

	// Debug: beim naechsten Frame das aktuelle Tiefenbild als Bild in den SKSE-Log-Ordner schreiben
	void RequestDepthDump() noexcept;
}
