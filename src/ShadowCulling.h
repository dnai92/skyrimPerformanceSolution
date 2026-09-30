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

	// Einmal pro Frame auf dem Main-Thread: Kamera-Position und Kaskaden-Culler der Sonne cachen
	void OnFrame();
}
