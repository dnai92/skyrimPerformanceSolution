#pragma once

// Hauptschalter per Taste (Standard Bild auf, [General] iToggleKey): schaltet alle Culling-Funktionen zur Laufzeit
// an/aus, zeigt eine HUD-Meldung und schreibt den Wechsel ins Log.
namespace Hotkey
{
	// Nach kDataLoaded (BSInputDeviceManager existiert dann)
	void Register();
}
