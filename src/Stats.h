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

		kTotal
	};

	inline constexpr std::array<const char*, static_cast<std::size_t>(Zone::kTotal)> kZoneNames{
		"Papyrus VM Update",
		"Papyrus Tasklets",
		"NPC Actor::Update",
		"Player Update",
	};

	// Zaehler pro Frame (Render-Aufrufe)
	enum class Counter : std::size_t
	{
		ColorDraws,      // Draws mit Render-Target (Hauptszene, UI, Post-Processing)
		DepthOnlyDraws,  // Draws nur mit Depth-Buffer (Shadow-Maps, Prepass, Occlusion)
		Dispatches,      // Compute-Shader (v. a. Community Shaders)

		kTotal
	};

	inline constexpr std::array<const char*, static_cast<std::size_t>(Counter::kTotal)> kCounterNames{
		"Draws Farbe",
		"Draws Depth-only",
		"Dispatches",
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
