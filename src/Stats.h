#pragma once

// Eigene, leichtgewichtige Frame-Statistik (unabhaengig von Tracy).
// Schreibt alle kReportInterval eine Zusammenfassung ins Log und eine Zeile in SPS.csv.
namespace Stats
{
	enum class Zone : std::size_t
	{
		PapyrusUpdate,
		PapyrusTasklets,
		NpcUpdate,
		PlayerUpdate,
		SunShadowAccumulate,  // BSShadowDirectionalLight::Accumulate (Culling/Eintragen)
		SunShadowRender,      // BSShadowDirectionalLight::Render (Draw Calls)
		FsmpWaitMain,         // Main-Thread wartet in hdtsmp64.dll (SwitchToThread/Sleep/Wait/Locks)
		CbpcWaitMain,         // Main-Thread wartet in cbp.dll
		GameWaitMain,         // Main-Thread wartet in SkyrimSE.exe selbst (Engine-Spin-Waits)
		LightGather,          // Licht-Geometrie-Zuordnung dynamischer Punktlichter (tatsaechlich ausgefuehrt)
		PrecipMask,           // Precipitation::SetupMask (Szenen-Durchlauf Regen-/Skylighting-Karte)
		MainCull,             // Szenen-Durchlauf der Hauptkamera (ID 32174)

		kTotal
	};

	inline constexpr std::array<const char*, static_cast<std::size_t>(Zone::kTotal)> kZoneNames{
		"Papyrus VM Update",
		"Papyrus Tasklets",
		"NPC Actor::Update",
		"Player Update",
		"Sonne Accumulate",
		"Sonne Render",
		"FSMP Warten (Main)",
		"CBPC Warten (Main)",
		"Engine Warten (Main)",
		"Licht-Zuordnung",
		"Regen/Sky-Karte",
		"Hauptkamera-Culling",
	};

	// Zaehler pro Frame (Meshes, die in Culling-Listen aufgenommen bzw. verworfen werden)
	enum class Counter : std::size_t
	{
		SunCascade0,      // Meshes in Sonnen-Kaskade 0 (nah)
		SunCascade1,      // Meshes in Sonnen-Kaskade 1
		SunCascade2Plus,  // Meshes in Sonnen-Kaskade 2+
		SunCulled,        // von ShadowCulling verworfene Meshes
		OtherCullers,     // AppendVirtual anderer Culler (Hauptszene, Punktlichter, ...)
		PointKept,        // Meshes in Punktlicht-Schattenkarten
		PointCulled,      // davon verworfen
		SkylightKept,     // Objekte in der Skylighting-/Niederschlags-Verdeckungskarte
		SkylightCulled,   // davon verworfen
		DepthKept,        // Draws im Tiefenvorpass (ueber gehookte Aufrufstelle)
		DepthCulled,      // davon uebersprungen
		DepthDrawsAll,    // alle Utility-Draws waehrend Main::RenderDepth (Kontrolle der Abdeckung)
		MainKept,         // Meshes im Culler der Hauptkamera
		MainCulled,       // davon per Mikro-Culling verworfen
		DecalKept,        // Decals in der Hauptszene
		DecalCulled,      // davon per Decal-Culling verworfen
		SunRenderCalls,   // Aufrufe von BSShadowDirectionalLight::Render
		SunDraws,         // Utility-SetupGeometry waehrend Sonnen-Render (= Draws)
		UtilityDraws,     // Utility-SetupGeometry insgesamt
		FsmpWaitCalls,    // Warte-Aufrufe von FSMP auf dem Main-Thread
		CbpcWaitCalls,    // Warte-Aufrufe von CBPC auf dem Main-Thread
		GameWaitCalls,    // Warte-Aufrufe der Engine auf dem Main-Thread
		CascadeSkipped,   // Meshes, die wegen Kaskaden-Cache nicht neu gezeichnet werden
		CascadeSkipFrames,// Frames, in denen die ferne Kaskade aus dem Cache kam (0/1)
		InstancedMeshes,  // per Instancing gezeichnete Meshes (ohne das jeweils erste der Gruppe)
		InstancedCalls,   // dafuer abgesetzte Instanced-Draw-Calls
		ActorShadowKept,  // geskinnte Meshes in Schattenkarten (Sonne + Punktlicht)
		ActorShadowCulled,// davon wegen Entfernung verworfen
		LightGatherCalls,   // Licht-Zuordnungen dynamischer Punktlichter (ausgefuehrt)
		LightGatherSkipped, // davon uebersprungen (Licht kaum bewegt)
		PrunedSun,          // im Sonnenschatten-Durchlauf uebersprungene Knoten (samt Inhalt)
		PrunedPoint,        // im Punktlicht-Durchlauf uebersprungene Knoten
		PrunedPrecip,       // im Niederschlags-/Skylighting-Durchlauf uebersprungene Knoten

		kTotal
	};

	inline constexpr std::array<const char*, static_cast<std::size_t>(Counter::kTotal)> kCounterNames{
		"Sonne Kaskade 0",
		"Sonne Kaskade 1",
		"Sonne Kaskade 2+",
		"Sonne gecullt",
		"Andere Culler",
		"Punktlicht behalten",
		"Punktlicht gecullt",
		"Skylight behalten",
		"Skylight gecullt",
		"Tiefenvorpass behalten",
		"Tiefenvorpass gecullt",
		"Tiefenvorpass Draws gesamt",
		"Hauptszene behalten",
		"Hauptszene gecullt",
		"Decals behalten",
		"Decals gecullt",
		"Sonne Render-Aufrufe",
		"Sonne Draws",
		"Utility Draws gesamt",
		"FSMP Warte-Aufrufe",
		"CBPC Warte-Aufrufe",
		"Engine Warte-Aufrufe",
		"Kaskaden-Cache Meshes gespart",
		"Kaskaden-Cache Frames",
		"Instancing Meshes",
		"Instancing Draw-Calls",
		"Charakter-Schatten behalten",
		"Charakter-Schatten gecullt",
		"Licht-Zuordnung ausgefuehrt",
		"Licht-Zuordnung gespart",
		"Knoten uebersprungen Sonne",
		"Knoten uebersprungen Punktlicht",
		"Knoten uebersprungen Regen/Sky",
	};

	void Init();

	void Add(Zone a_zone, std::int64_t a_ns) noexcept;
	void Count(Counter a_counter) noexcept;
	void CountNpcUpdate() noexcept;
	void OnOverstressed() noexcept;
	void OnCellLoaded() noexcept;

	// Frame-Grenze: wird zu Beginn von PlayerCharacter::Update aufgerufen (1x pro Spiel-Frame)
	void OnFrame() noexcept;

	class ScopedTimer
	{
	public:
		explicit ScopedTimer(Zone a_zone) noexcept :
			_zone(a_zone), _start(std::chrono::steady_clock::now()) {}

		~ScopedTimer() noexcept
		{
			const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _start).count();
			Add(_zone, ns);
		}

		ScopedTimer(const ScopedTimer&) = delete;
		ScopedTimer& operator=(const ScopedTimer&) = delete;

	private:
		Zone                                  _zone;
		std::chrono::steady_clock::time_point _start;
	};
}
