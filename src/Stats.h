#pragma once

// Eigene, leichtgewichtige Frame-Statistik (unabhaengig von Tracy).
// Schreibt alle kReportInterval eine Zusammenfassung ins Log und eine Zeile in SkyrimPerf.csv.
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

		kTotal
	};

	inline constexpr std::array<const char*, static_cast<std::size_t>(Zone::kTotal)> kZoneNames{
		"Papyrus VM Update",
		"Papyrus Tasklets",
		"NPC Actor::Update",
		"Player Update",
		"Sonne Accumulate",
		"Sonne Render",
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
		SunRenderCalls,   // Aufrufe von BSShadowDirectionalLight::Render
		SunDraws,         // Utility-SetupGeometry waehrend Sonnen-Render (= Draws)
		UtilityDraws,     // Utility-SetupGeometry insgesamt

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
		"Sonne Render-Aufrufe",
		"Sonne Draws",
		"Utility Draws gesamt",
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
