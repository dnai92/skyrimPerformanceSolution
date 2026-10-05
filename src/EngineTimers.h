#pragma once

// Zusaetzliche Zeitmessungen fuer den 10-s-Bericht (A/B-Vergleiche im Spiel ueber das Menue):
//   Regen/Sky-Karte     = Precipitation::SetupMask (ID 26183), Szenen-Durchlauf fuer die Niederschlags-/Skylighting-Karte
//   Hauptkamera-Culling = ID 32174 (aus der Render-Funktion ID 36560), Szenen-Durchlauf der Hauptkamera
namespace EngineTimers
{
	void Install();
}
