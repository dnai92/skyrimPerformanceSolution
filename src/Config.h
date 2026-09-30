#pragma once

// Einstellungen aus Data\SKSE\Plugins\SkyrimPerf.ini (fehlende Werte -> Defaults)
namespace Config
{
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
	inline CullRule pointLightCulling{ true, 0, 1000.0f, 150.0f, 0.035f, true };  // Punktlichter [PointLightShadowCulling]
	inline CullRule depthPrepassCulling{ true, 0, 1500.0f, 200.0f, 0.025f, true }; // Tiefenvorpass [DepthPrepassCulling]

	struct SkylightingCulling
	{
		bool  enabled = true;
		float minRadius = 128.0f;  // Community Shaders nimmt selbst nur Objekte ab Radius 32 auf
	};
	inline SkylightingCulling skylightingCulling;

	void Load();

	// Prueft (alle ~2 s, Main-Thread) ob die INI geaendert wurde und laedt sie dann neu
	void ReloadIfChanged();
}
