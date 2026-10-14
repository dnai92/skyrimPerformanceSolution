#pragma once

#include <cstddef>
#include <cstdint>

// Cache der fernen Sonnenkaskade, GPU-Teil (eigene Uebersetzungseinheit ohne CommonLib-PCH, da <d3d11.h>).
// Je Schattenkarten-Ziel (Sonne, Volumetric Lighting von Community Shaders) zwei eigene Kopien einer Ebene:
// "prev" = vollstaendige Karte des Vorframes (Aufbau ohne Blinken), "static" = nur unbewegliche Schattenwerfer.
// Im Cache-Frame zeichnet die Engine nur Bewegliches; danach wird "static" per Tiefentest darueber gelegt
// (naeherer Wert gewinnt - wie beim normalen Zeichnen in die Schattenkarte).
namespace FarShadowGpu
{
	enum Slot : std::uint32_t
	{
		kPrev = 0,
		kStatic = 1
	};

	// Einmalig (Main-Thread). false + Fehlertext bei Problemen.
	bool Init(void* a_device, char* a_error, std::size_t a_errorSize) noexcept;
	bool Ready() noexcept;

	// Eigene Kopien fuer Ziel a_target (0 = Sonne, 1 = Volumetric) passend zu a_texture anlegen. true = vorhanden;
	// a_recreated = neu angelegt (Inhalt ungueltig).
	bool Ensure(std::uint32_t a_target, void* a_texture, bool& a_recreated) noexcept;

	// Ebene a_slice (Mip 0) von a_texture in Kopie a_slot sichern bzw. aus ihr zuruecklegen (ganze Ebene).
	void Save(void* a_context, std::uint32_t a_target, std::uint32_t a_slot, void* a_texture, std::uint32_t a_slice) noexcept;
	void Restore(void* a_context, std::uint32_t a_target, std::uint32_t a_slot, void* a_texture, std::uint32_t a_slice) noexcept;

	// Kopie a_slot per Tiefentest in die Ebene (Depth-Stencil-View a_dsv) zeichnen. Stellt den Zustand wieder her.
	bool Merge(void* a_context, std::uint32_t a_target, std::uint32_t a_slot, void* a_dsv) noexcept;

	// Passt die Depth-Stencil-View zu Ebene a_slice von a_texture (beschreibbar)? a_info: Kurzbeschreibung fuers Log.
	bool DsvMatches(void* a_dsv, void* a_texture, std::uint32_t a_slice, char* a_info, std::size_t a_infoSize) noexcept;
	// Gleich aufgebaute Texturen (Groesse, Format, Ebenen)?
	bool SameLayout(void* a_a, void* a_b) noexcept;
	// Ebene a_slice (Mip 0) von a_src nach a_dst kopieren
	void CopySlice(void* a_context, void* a_dst, void* a_src, std::uint32_t a_slice) noexcept;

	// Diagnose (CPU-Auslese, blockiert kurz): wie viele Texel der Ebene sind belegt (nicht 0 und nicht Maximalwert)?
	struct ProbeResult
	{
		std::uint32_t covered = 0, zero = 0, full = 0;
		std::uint32_t minV = 0, maxV = 0;  // ueber belegte Texel, Rohwert
		bool          ok = false;
	};
	// a_texture != nullptr: Ebene a_slice dieser Textur; sonst eigene Kopie a_slot von Ziel a_target
	ProbeResult Probe(void* a_context, void* a_texture, std::uint32_t a_slice, std::uint32_t a_target, std::uint32_t a_slot) noexcept;
	std::uint32_t DepthFunc() noexcept;

	// Vergleichsfunktion der Engine fuer Schattenkarten aus dem gerade gesetzten Tiefenzustand lernen (einmalig,
	// waehrend des Sonnen-Zeichnens). Unbekannt = LESS_EQUAL (Standard-Tiefe).
	bool DepthFuncKnown() noexcept;
	void NoteDepthState(void* a_context) noexcept;

	// Alles freigeben (Ladebildschirm / Funktion aus)
	void Reset() noexcept;
}
