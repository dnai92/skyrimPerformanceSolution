#pragma once

// Einstellungen aus Data\SKSE\Plugins\SkyrimPerf.ini (fehlende Werte -> Defaults)
namespace Config
{
	// Hauptschalter (Taste, siehe Hotkey) - wirkt auf ALLE Culling-Funktionen, unabhaengig von den einzelnen bEnabled
	inline std::atomic<bool>          masterEnabled{ true };
	inline std::atomic<std::uint32_t> toggleKey{ 0xC7 };  // DirectInput-Scancode, 0xC7 = Pos1 (Home)

	// Regel fuer das Kleinobjekt-Culling in einer Schattenkarte. Ein Mesh wird verworfen, wenn ALLE Bedingungen zutreffen.
	struct CullRule
	{
		bool          enabled = true;
		std::uint32_t minCascade = 1;         // nur Sonne: Kaskaden darunter bleiben unangetastet
		float         minDistance = 1500.0f;  // naeher an der Kamera -> immer Schatten
		float         maxRadius = 150.0f;     // groessere Objekte -> immer Schatten
		float         minAngularSize = 0.02f; // Radius/Distanz darunter -> kein Schatten mehr
		bool          skipSkinned = true;     // Charaktere/Kreaturen nie cullen
	};

	inline CullRule shadowCulling;                                               // Sonne [ShadowCulling]
	inline std::atomic<float> sunMinElevation{ 25.0f };  // Sonne tiefer (Grad) -> kein Sonnen-Culling (lange Schatten) [ShadowCulling] fMinSunElevation
	inline CullRule pointLightCulling{ true, 0, 1000.0f, 150.0f, 0.035f, true };  // Punktlichter [PointLightShadowCulling]
	inline CullRule depthPrepassCulling{ false, 0, 1500.0f, 200.0f, 0.025f, true }; // Tiefenvorpass [DepthPrepassCulling]

	struct SkylightingCulling
	{
		bool  enabled = true;
		float minRadius = 128.0f;  // Community Shaders nimmt selbst nur Objekte ab Radius 32 auf
	};
	inline SkylightingCulling skylightingCulling;

	// Hauptszene: winzige, weit entfernte Objekte gar nicht zeichnen (standardmaessig AUS - Aufploppen moeglich)
	inline CullRule mainViewCulling{ false, 0, 3000.0f, 100.0f, 0.002f, true };  // [MainViewCulling]

	struct DecalCulling
	{
		bool  enabled = true;
		float maxDistance = 1500.0f;  // Decals (Fussabdruecke, Blut, Schmutz) weiter weg werden nicht gezeichnet
		float maxRadius = 100.0f;     // nur kleine Decals
	};
	inline DecalCulling decalCulling;

	// Ferne Sonnenkaskade nur jeden n-ten Frame neu zeichnen, dazwischen alte Schattenkarte weiterverwenden
	struct CascadeCache
	{
		bool          enabled = true;
		std::uint32_t cascade = 1;   // ab dieser Kaskade (0 = nah)
		std::uint32_t interval = 2;  // 2 = jeden 2. Frame neu zeichnen
		// Diagnose-Schalter (einzeln abschaltbar, um Flackern einzugrenzen)
		bool freezeMatrix = true;       // im Cache-Frame alte lightTransform einsetzen
		bool noClear = true;            // im Cache-Frame clearRenderTarget = false
		bool restoreShadowmap = true;   // Sonnen-Schattenkarte per GPU-Kopie sichern/zuruecklegen
		bool restoreVolumetric = true;  // Volumetric-Schattenkarte (CS) per GPU-Kopie sichern/zuruecklegen
	};
	inline CascadeCache cascadeCache;  // [ShadowCascadeCache]

	void Load();

	// Prueft (alle ~2 s, Main-Thread) ob die INI geaendert wurde und laedt sie dann neu
	void ReloadIfChanged();
}
