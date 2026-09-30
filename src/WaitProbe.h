#pragma once

#include <cstddef>
#include <cstdint>

// Misst, wie lange der Main-Thread in Warte-Funktionen fremder Physik-DLLs (FSMP hdtsmp64.dll, CBPC cbp.dll)
// verbringt. Dazu werden die Import-Eintraege (IAT) dieser DLLs fuer SwitchToThread, Sleep, WaitForSingleObjectEx,
// SleepConditionVariableSRW, AcquireSRWLockExclusive und EnterCriticalSection auf Messfunktionen umgebogen.
// Worker-Threads laufen unveraendert durch (nur eine Thread-ID-Abfrage).
namespace WaitProbe
{
	struct ModuleResult
	{
		bool found = false;  // DLL geladen
		int  patched = 0;    // umgebogene IAT-Eintraege
	};

	// Nach kPostLoad aufrufen (alle SKSE-DLLs geladen), auf dem Main-Thread.
	// a_game: SkyrimSE.exe selbst (Spin-Waits der Engine, z. B. Warten auf Job-Threads)
	void Install(ModuleResult& a_fsmp, ModuleResult& a_cbpc, ModuleResult& a_game) noexcept;

	// Engine-Wartestellen nach Aufrufer (Ruecksprungadresse als RVA in SkyrimSE.exe), nur Main-Thread
	struct CallerStat
	{
		std::uintptr_t rva = 0;
		std::uint64_t  ns = 0;
		std::uint64_t  calls = 0;
	};
	inline constexpr std::size_t kMaxCallers = 32;

	// Kopiert die Tabelle (seit dem letzten Aufruf) und setzt sie zurueck; nur vom Main-Thread aufrufen
	std::size_t TakeCallers(CallerStat* a_out, std::size_t a_max) noexcept;
}
