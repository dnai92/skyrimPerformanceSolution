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
	// a_prepass: Draw im Tiefenvorpass (sonst Hauptbild). Erst nach Beginn des Vorpasses markierte Objekte laesst der
	// Vorpass nie aus, das Hauptbild nur ohne occlEarlyMarksOnly (Messung 1.0.66)
	bool IsHidden(const RE::BSGeometry* a_geom, bool a_prepass) noexcept;
	void CountFilterSkip() noexcept;
	// Main::RenderDepth beginnt (vor jedem Draw des Vorpasses, unabhaengig vom Verdeckungs-Schalter)
	void OnPrepassStart() noexcept;

	// Ladebildschirm / Hauptmenue
	void Reset() noexcept;

	// Debug: beim naechsten Frame das aktuelle Tiefenbild als Bild in den SKSE-Log-Ordner schreiben
	void RequestDepthDump() noexcept;
}
