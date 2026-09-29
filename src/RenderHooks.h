#pragma once

namespace RenderHooks
{
	// Zaehlt Draw-/Dispatch-Aufrufe pro Frame ueber vtable-Hooks auf dem D3D11-Immediate-Context des Spiels.
	// "Depth-only" = Draws ohne gebundenes Render-Target, nur mit Depth-Buffer (Shadow-Maps, Depth-Prepass,
	// CS-Skylighting-Occlusion u. ae.). Muss nach Renderer-Init installiert werden (kDataLoaded).
	void Install();
}
