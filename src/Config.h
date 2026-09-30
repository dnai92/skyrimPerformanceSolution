#pragma once

// Einstellungen aus Data\SKSE\Plugins\SkyrimPerf.ini (fehlende Werte -> Defaults)
namespace Config
{
	struct ShadowCulling
	{
		bool          enabled = true;
		std::uint32_t minCascade = 1;         // Kaskaden darunter bleiben unangetastet (0 = naechste Kaskade)
		float         minDistance = 1500.0f;  // naeher an der Kamera -> immer Schatten
		float         maxRadius = 150.0f;     // groessere Objekte (Gebaeude, Mauern) -> immer Schatten
		float         minAngularSize = 0.02f; // Radius/Distanz darunter -> kein Schatten mehr
		bool          skipSkinned = true;     // Charaktere/Kreaturen nie cullen
	};

	inline ShadowCulling shadowCulling;

	void Load();

	// Prueft (alle ~2 s, Main-Thread) ob die INI geaendert wurde und laedt sie dann neu
	void ReloadIfChanged();
}
