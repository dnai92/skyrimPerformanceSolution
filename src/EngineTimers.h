#pragma once

// Zusaetzliche Zeitmessungen fuer den 10-s-Bericht (A/B-Vergleiche im Spiel ueber das Menue):
//   Regen/Sky-Karte     = Precipitation::SetupMask (ID 26183), Szenen-Durchlauf fuer die Niederschlags-/Skylighting-Karte
//   Hauptkamera-Culling = ID 32174 (aus der Render-Funktion ID 36560), Szenen-Durchlauf der Hauptkamera
namespace EngineTimers
{
	void Install();

	// A/B-Messung Himmelslicht-Culling (Debug-Seite): 8 Abschnitte a 15 s, abwechselnd an/aus, je Abschnitt Zeit von
	// Precipitation::SetupMask und gezeichnete/weggelassene Objekte. Braucht die Zeitmessung (bEngineProbes).
	bool PrecipTimerInstalled() noexcept;
	void StartSkylightAB() noexcept;
	bool SkylightABRunning() noexcept;
	void CountSkylight(bool a_culled) noexcept;  // aus dem Skylighting-Hook, nur waehrend der Messung
	void OnFrame();                              // Main-Thread, je Frame im Spiel

	// A/B-Messung der Beta-Funktionen (Debug-Seite, ohne Entwickler-Schalter): 12 Abschnitte a 15 s reihum
	// aus / nur Verdeckung / nur Schatten-Cache / beide, je Abschnitt Frame-Zeit und NPC-Updates
	void StartBetaAB() noexcept;
	bool BetaABRunning() noexcept;
}
