#pragma once

#include <cstdint>

// GPU-Zeitmessung per D3D11-Timestamp-Queries (eigene Uebersetzungseinheit ohne CommonLib-PCH, da <d3d11.h>).
// Nur mit Analyse-Protokoll aktiv. Gemessen wird auf dem Immediate Context des Main-Threads (= Render-Thread):
// Frame (von Frame-Grenze zu Frame-Grenze, inkl. Leerlauf der GPU), und darin einzelne Abschnitte. Ergebnisse kommen
// einige Frames verspaetet zurueck (keine Wartezeit fuer die CPU).
namespace GpuTimer
{
	enum Section : int
	{
		kSunShadows = 0,    // BSShadowDirectionalLight::Render
		kLightShadows = 1,  // Render der Punkt-/Spotlicht-Schatten (Parabolic, Frustum)
		kDepthPrepass = 2,  // Main::RenderDepth

		kSectionCount
	};

	inline constexpr const char* kSectionNames[kSectionCount]{ "sun shadows", "light shadows", "depth pre-pass" };

	// Frame-Grenze (Main-Thread, einmal pro Frame). a_device/a_context: ID3D11Device*/ID3D11DeviceContext* (Immediate)
	void FrameBoundary(void* a_device, void* a_context, bool a_enabled) noexcept;

	// Abschnitt im aktuellen Frame (nur auf dem Thread der Frame-Grenze, sonst ignoriert)
	void Begin(Section a_section) noexcept;
	void End(Section a_section) noexcept;

	struct Window
	{
		std::uint32_t frames = 0;
		double        frameAvg = 0, frameMax = 0;
		double        sectionAvg[kSectionCount]{};
		double        callsPerFrame[kSectionCount]{};
		std::uint32_t disjoint = 0;  // verworfene Frames (Takt der GPU geaendert)
		std::uint32_t skipped = 0;   // nicht gemessen (GPU zu weit hinten / zu viele Abschnitte)
	};

	// Mittelwerte seit dem letzten Aufruf (gleicher Thread wie FrameBoundary)
	Window Take() noexcept;
}
