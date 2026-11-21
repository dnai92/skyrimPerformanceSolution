#pragma once

// Grafikspeicher des eigenen Prozesses laut Windows-Leistungszaehler "GPU Process Memory" (wie im Task-Manager).
// Anders als IDXGIAdapter3::QueryVideoMemoryInfo enthaelt er ALLE Grafik-Objekte des Prozesses (auch die von
// Community Shaders / DLSS / Frame Generation) und den ausgelagerten Anteil (Shared Usage). Gemessen 0.20.3 am Markt:
// DXGI 8,3-9,7 GB, Zaehler 10,9-11,1 GB + 150-330 MB ausgelagert.
// Eigene Uebersetzungseinheit ohne CommonLib-PCH (braucht <Windows.h>/<pdh.h>). Nur aus EINEM Thread aufrufen.
namespace GpuMemory
{
	// false = Zaehler nicht verfuegbar
	bool Query(unsigned long long& a_dedicated, unsigned long long& a_shared) noexcept;

	// Where the values come from (for the log): the Windows counter, or under Wine/Proton the Linux kernel's DRM
	// statistics of the process. Valid after the first Query.
	const char* Source() noexcept;
}
