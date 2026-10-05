#pragma once

// Menue im SKSE Menu Framework (Abschnitt "SkyrimPerf"): alle Optimierungen zur Laufzeit ein-/ausschalten und einstellen.
// Aenderungen wirken sofort und werden (verzoegert, Main-Thread) in SkyrimPerf_User.ini gespeichert.
namespace Menu
{
	// Nach kDataLoaded aufrufen (das Framework ist dann geladen)
	void Register();
}
