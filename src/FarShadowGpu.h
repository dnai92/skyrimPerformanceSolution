#pragma once

#include <cstddef>
#include <cstdint>

// Cache der fernen Sonnenkaskade, GPU-Teil (eigene Uebersetzungseinheit ohne CommonLib-PCH, da <d3d11.h>).
// Zwei eigene Kopien einer Ebene der Sonnen-Schattenkarte: "prev" = vollstaendige Ebene des Vorframes (Aufbau ohne
// Blinken), "static" = nur unbewegliche Schattenwerfer. Im Cache-Frame wird "static" nach dem Leeren eingesetzt und
// die Engine zeichnet nur Bewegliches darueber.
namespace FarShadowGpu
{
	enum Slot : std::uint32_t
	{
		kPrev = 0,
		kStatic = 1
	};

	// Eigene Kopien passend zu a_texture anlegen. true = vorhanden; a_recreated = neu angelegt (Inhalt ungueltig).
	bool Ensure(void* a_texture, bool& a_recreated) noexcept;

	// Ebene a_slice (Mip 0) von a_texture in Kopie a_slot sichern bzw. aus ihr zuruecklegen
	void Save(void* a_context, std::uint32_t a_slot, void* a_texture, std::uint32_t a_slice) noexcept;
	void Restore(void* a_context, std::uint32_t a_slot, void* a_texture, std::uint32_t a_slice) noexcept;

	// Ist gerade genau Ebene a_slice von a_texture als (beschreibbarer) Tiefenpuffer gebunden?
	bool BoundSlice(void* a_context, void* a_texture, std::uint32_t a_slice) noexcept;

	// Alles freigeben
	void Reset() noexcept;
}
