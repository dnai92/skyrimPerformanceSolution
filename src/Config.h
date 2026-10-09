#pragma once

// Einstellungen aus Data\SKSE\Plugins\SPS.ini (fehlende Werte -> Defaults)
namespace Config
{
	// Hauptschalter (Taste, siehe Hotkey) - wirkt auf ALLE Culling-Funktionen, unabhaengig von den einzelnen bEnabled
	inline std::atomic<bool>          masterEnabled{ true };
	inline bool                       engineProbes = false;  // nur INI, nur beim Start: WaitProbe + Engine-Zeitmessung (Entwickler)
	inline std::atomic<bool>          analysis{ false };  // Diagnose-Auswertungen (Instancing-Analyse, Decal-/Culled-Listen, Kamera-Zaehler). Kosten Hauptthread-Zeit -> Standard AUS
	inline std::atomic<std::uint32_t> toggleKey{ 0xC9 };  // DirectInput-Scancode, 0xC9 = Bild auf (Page Up)

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
	// Sonnenschatten, die das Sichtfeld nicht erreichen koennen, weglassen - ZURUECKGESTELLT (1.0.17): ganze Knoten mit
	// NPCs wurden wegen veralteter Figuren-Huellen weggelassen (fehlende NPC-Schatten mitten im Bild), einzelne Meshes
	// bringen nur 2-25 pro Frame. Nicht mehr per INI/Menue schaltbar.
	inline std::atomic<bool>  sunViewCulling{ false };
	inline std::atomic<float> sunMinElevation{ 10.0f };  // Sonne tiefer (Grad) -> kein Sonnen-Culling (lange Schatten) [ShadowCulling] fMinSunElevation
	inline CullRule pointLightCulling{ true, 0, 1000.0f, 150.0f, 0.035f, true };  // Punktlichter [PointLightShadowCulling]
	// Innenraeume werden fast nur von Punktlichtern beleuchtet -> fehlende Schatten fallen dort auf (Drachenfeste)
	inline std::atomic<bool> pointLightInteriors{ false };  // [PointLightShadowCulling] bInteriors
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
		float maxRadius = 32.0f;      // nur kleine Decals (Fussabdruecke ~22); groesser traf Putz-/Steinflecken an Mauern
	};
	inline DecalCulling decalCulling;

	// Ferne Sonnenkaskade nur jeden n-ten Frame neu zeichnen, dazwischen alte Schattenkarte weiterverwenden
	struct CascadeCache
	{
		bool          enabled = false;  // Standard AUS: Flackern bei tiefer Sonne (siehe INI)
		std::uint32_t cascade = 1;   // ab dieser Kaskade (0 = nah)
		std::uint32_t interval = 2;  // 2 = jeden 2. Frame neu zeichnen
		// Diagnose-Schalter (einzeln abschaltbar, um Flackern einzugrenzen)
		bool freezeMatrix = true;       // im Cache-Frame alte lightTransform einsetzen
		bool freezeCamera = true;       // im Cache-Frame Kamera + clipPlanes einfrieren; Engine-Stand vor naechstem Accumulate zurueck
		bool freezeSplits = true;       // im Cache-Frame Kaskaden-Grenzen, isEnabled und Port einfrieren
		bool skipDraws = true;          // Diagnose: false = ferne Kaskade im Cache-Frame trotzdem zeichnen (keine Ersparnis)
		bool requireSameProjection = true;  // nur cachen, wenn die Engine exakt dieselbe Kaskaden-Projektion berechnet
		float projectionEpsilon = 0.0001f;  // erlaubte Abweichung (worldToCam/Frustum)
		bool noClear = true;            // im Cache-Frame clearRenderTarget = false
		bool restoreShadowmap = true;   // Sonnen-Schattenkarte per GPU-Kopie sichern/zuruecklegen
		bool restoreVolumetric = true;  // Volumetric-Schattenkarte (CS) per GPU-Kopie sichern/zuruecklegen
	};
	inline CascadeCache cascadeCache;  // [ShadowCascadeCache]

	// Stabile ferne Sonnenkaskade (Stufe 1 fuer den Kaskaden-Cache): Lage, Ausschnitt und Tiefenbereich der fernen
	// Kaskade aendern sich nur noch in Schritten statt in jedem Frame (Engine passt sie sonst laufend an Blick und Sonne an)
	struct StableCascade
	{
		bool  enabled = false;        // Standard AUS (Test)
		float maxAngleDeg = 0.05f;    // Sonnenrichtung erst nachfuehren, wenn sie um mehr als diesen Winkel abweicht (0,15 = sichtbare Spruenge)
		// Reserven klein halten: der Tiefen-Versatz der Engine (Shadow Bias) waechst mit Tiefenbereich und Texelgroesse -
		// mit ~1,4x groesserem Ausschnitt/Bereich fehlten kurze Schatten (Dachueberstand auf Hauswand, Test 1.0.10)
		// Auch 1,1x sind zu viel: Community Shaders bemisst weiche Schatten am Tiefenabstand relativ zum Bereich der
		// Kaskade - jede Vergroesserung macht kurze Schatten blasser (Diagnose-Modi 4-7). Deshalb nur winzige Toleranzen:
		// festgehalten wird nur gegen das Zittern der Engine-Werte im Stand.
		float extentStep = 64.0f;     // Ausschnitt-Groesse in diesen Schritten (Einheiten)
		float extentFactor = 1.0f;    // Ausschnitt = (Engine-Ausschnitt + margin) * Faktor
		float margin = 48.0f;         // Spielraum um den Engine-Ausschnitt, bevor neu ausgerichtet wird
		float depthStep = 64.0f;      // Tiefen-Spielraum je Seite (Einheiten)
		// Diagnose (Menue): 0 normal | 1 nur rechnen, nichts setzen | 2 Engine-Werte ueber unseren Weg setzen (Update) |
		// 3 normal ohne Kamera-Update | 4 normal mit Engine-Tiefenbereich | 5 ohne Festhalten von Drehung/Position
		float debugMode = 0.0f;
	};
	inline StableCascade stableCascade;  // [ShadowStableCascade]

	// Instancing der Sonnenschatten (Schritt 2): gleiche einfache Meshes eines Batches in einem Draw Call
	struct ShadowInstancing
	{
		bool          enabled = false;        // im Markt-Test nur ~0,1 ms; ab 1.0.6 Standard aus (zeichnet selbst - Flackern auf GOG gemeldet)
		bool          verify = false;         // Pruefmodus: Engine zeichnet alles, Instanzen werden ZUSAETZLICH gezeichnet
		float         debugOffsetZ = 0.0f;    // Pruefmodus: Instanzen um diesen Wert nach oben versetzen (sichtbarer Beweis)
		std::uint32_t technique = 0xC046;     // erste Technik der Liste (Kompatibilitaet)
		std::array<std::uint32_t, 16> techniques{ 0xC046 };  // erlaubte Utility-Techniken (Shadowmap ohne Alpha-Test)
		std::uint32_t techniqueCount = 1;
		bool          allowTwoSided = false;  // zweiseitige Meshes (anderer Rasterizer-Zustand) mit instanzieren
		std::uint32_t minGroup = 2;           // ab so vielen gleichen Meshes pro Batch
		std::uint32_t debugMode = 0;          // Diagnose: 1 = Flush ohne Draw-Aufrufe (nur Zustand), 2 = gar kein Flush, 3 = nur sammeln/zaehlen
	};
	inline ShadowInstancing shadowInstancing;  // [ShadowInstancing]

	// Schatten entfernter Charaktere/Kreaturen (geskinnte Meshes: Koerper, Kleidung, Haare) weglassen
	struct ActorShadowCulling
	{
		bool  enabled = true;
		float minDistance = 3500.0f;  // ab dieser Entfernung zur Kamera (~50 m)
		static constexpr float kMinAllowed = 2100.0f;  // nie unter ~30 m (Nutzerwunsch)
		bool  pointLights = true;      // auch in Punktlicht-Schatten (Fackeln)
	};
	inline ActorShadowCulling actorShadowCulling;  // [ActorShadowCulling]

	// Licht-Geometrie-Zuordnung dynamischer Punktlichter nicht in jedem Frame neu berechnen (siehe LightGather.h)
	struct LightGather
	{
		bool  enabled = true;
		float minMove = 8.0f;           // Licht muss sich mindestens so weit bewegt haben (Spielereinheiten) ...
		float minRadiusChange = 16.0f;  // ... oder der Radius sich so stark geaendert haben ...
		float maxAgeMs = 500.0f;        // ... oder die letzte Suche so lange her sein (500 im Test unauffaellig)
	};
	inline LightGather lightGather;  // [LightGatherThrottle]

	// Ganze Teilbaeume (Knoten samt Inhalt) im Culling-Durchlauf ueberspringen, wenn schon die Huelle des Knotens
	// die jeweilige Culling-Regel erfuellt - Ergebnis identisch, spart den Besuch aller Kinder.
	struct SubtreePruning
	{
		bool enabled = true;
		bool sun = true;      // Sonnen-/Mondschatten
		bool point = false;   // Punktlicht-Schatten: verdunkelte im Test beleuchtete Flaechen (Durchlauf sammelt wohl auch Beleuchtete)
		bool precip = true;   // Niederschlags-/Skylighting-Verdeckungskarte
	};
	inline SubtreePruning subtreePruning;  // [SubtreePruning]

	// Texture Streaming (siehe TextureStream.h)
	struct TextureStream
	{
		bool        enabled = true;       // Stufe 1: ferne Texturen im VRAM verkleinern / bei Annaeherung neu laden
		bool        analysis = false;     // ausfuehrlicher 10-s-Bericht im Log (sonst eine Zeile pro Minute)
		bool        loadReduced = true;   // Stufe 3: gemerkte Groesse schon beim Laden anwenden
		bool        budgetMode = true;    // nur verkleinern, wenn der VRAM knapp wird (Windows-Budget des Prozesses)
		float       budgetStartPct = 85.0f;  // ab dieser Belegung (% des Budgets) wird verkleinert
		bool        refill = true;        // Budget-Modus: VRAM wieder auffuellen (verkleinerte Texturen voll laden), wenn Platz ist
		float       refillGapPct = 10.0f; // ... sobald die Belegung so viele %-Punkte unter der Schwelle liegt
		float       ramCacheMB = 1024.0f; // RAM-Puffer fuer neu geladene Texturdaten (0 = aus)
		float       safetyFactor = 2.0f;  // benoetigte Aufloesung = Bildschirmgroesse x Faktor
		float       minEdge = 1024.0f;    // nie unter diese Kantenlaenge verkleinern (Zweierpotenz)
		float       maxEdge = 0.0f;       // Obergrenze fuer alle gestreamten Texturen (1024/2048/4096/8192, 0 = keine)
		float       budgetMs = 0.4f;      // Zeit pro Frame fuer den Szenen-Durchlauf
		bool        streamClothing = true;     // Kleidung/Ruestung an Figuren verkleinern (Abstand ueber die Figur, nicht ihre Huelle)
		bool        streamCharacters = false;  // Koerper, Gesichter, Haare (textures/actors/character/) verkleinern
		std::string exclude = "interface\\,\\maps\\,mapmarker,\\lod\\,terrain\\,fonts\\,book,\\sky\\,effects\\,cubemaps\\";  // Pfadteile (kleingeschrieben), nie verkleinern
	};
	inline TextureStream textureStream;  // [TextureStream]

	// Schatten-Cache fester Lichter (siehe LightShadowCache.h)
	struct LightShadowCache
	{
		bool  enabled = true;
		float debugMode = 0;    // Pruef-Modus: 1 nie neu aufbauen, 2 Aufbau ohne Vorframe-Karte, 3 zurueckkopieren + alles zeichnen
	};
	inline LightShadowCache lightShadowCache;  // [LightShadowCache]

	// Laedt SPS.ini (Vorgaben aus dem Mod-Paket) und darueber SPS_User.ini (Werte aus dem Menue im Spiel)
	void Load();

	// Prueft (alle ~2 s, Main-Thread) ob eine der INIs geaendert wurde und laedt sie dann neu.
	// Schreibt ausserdem vorgemerkte Menue-Aenderungen in SPS_User.ini.
	void ReloadIfChanged();

	// Menue: Werte wurden direkt geaendert -> beim naechsten ReloadIfChanged in SPS_User.ini speichern
	void MarkDirty() noexcept;

	// Menue: SPS_User.ini loeschen und die Vorgaben aus SPS.ini neu laden (beim naechsten ReloadIfChanged)
	void RequestReset() noexcept;
}
