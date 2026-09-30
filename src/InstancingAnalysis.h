#pragma once

// Analyse (veraendert NICHTS am Rendering): Wie viele Draws im Sonnenschatten-Pass liessen sich per
// Instancing zusammenfassen? Gruppiert jeden Draw nach (Vertex-Buffer, Index-Buffer, Technik, Alpha-Test)
// und schreibt alle ~600 Frames eine Auswertung ins Log.
namespace InstancingAnalysis
{
	// Braucht den SKSE-Trampolin (write_call)
	void Install();

	// Einmal pro Frame (Main-Thread)
	void OnFrame();
}
