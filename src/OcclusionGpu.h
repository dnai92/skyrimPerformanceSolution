#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Verdeckungs-Experiment, GPU-Teil (eigene Uebersetzungseinheit ohne CommonLib-PCH, da <d3d11.h>).
// Ein Compute-Shader fasst einen Tiefenpuffer auf kWidth x kHeight Kacheln zusammen (je Kachel naechster und
// fernster Wert), das Ergebnis kommt ueber einen Ring aus Staging-Texturen ohne Warten zur CPU zurueck.
namespace OcclusionGpu
{
	constexpr std::uint32_t kWidth = 256;
	constexpr std::uint32_t kHeight = 144;
	constexpr std::uint32_t kRing = 3;

	struct Readback
	{
		std::vector<float> minDepth;  // kWidth * kHeight, zeilenweise
		std::vector<float> maxDepth;
		std::uint32_t      tag = 0;     // vom Aufrufer bei Capture mitgegeben (Kamera-Schnappschuss)
		std::uint32_t      srcW = 0, srcH = 0;
	};

	// Einmalig (Main-Thread, Device vorhanden). false + Fehlertext bei Problemen.
	bool Init(void* a_device, char* a_error, std::size_t a_errorSize) noexcept;
	bool Ready() noexcept;

	// Tiefenpuffer (Shader-Resource-View) zusammenfassen und in den Ring kopieren. Stellt den Compute-Zustand
	// danach wieder her. false = Ring voll (CPU kommt nicht nach) oder SRV unbrauchbar.
	bool Capture(void* a_context, void* a_depthSRV, std::uint32_t a_tag) noexcept;

	// Aeltesten fertigen Eintrag abholen, ohne zu warten. true = a_out wurde gefuellt.
	bool Poll(void* a_context, Readback& a_out) noexcept;

	// Alles verwerfen (Ladebildschirm)
	void Reset() noexcept;
}
