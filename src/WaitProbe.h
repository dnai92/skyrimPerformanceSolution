#pragma once

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

	// Nach kPostLoad aufrufen (alle SKSE-DLLs geladen), auf dem Main-Thread
	void Install(ModuleResult& a_fsmp, ModuleResult& a_cbpc) noexcept;
}
