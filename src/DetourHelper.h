#pragma once

// Duenne Huelle um Microsoft Detours (eigene Uebersetzungseinheit ohne CommonLib-PCH, da detours.h <Windows.h> braucht).
// Detours kann sich an Funktionen haengen, die bereits von anderen Mods (z. B. Community Shaders) per Detours
// umgeleitet wurden; die Hooks werden dann nacheinander ausgefuehrt.
namespace DetourHelper
{
	// a_target: Zeiger auf die Adresse der Zielfunktion; wird bei Erfolg auf das Trampolin (Original) umgesetzt.
	// Rueckgabe: Detours-Fehlercode (0 = NO_ERROR)
	long Attach(void** a_target, void* a_detour) noexcept;
}
