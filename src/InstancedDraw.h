#pragma once

#include <cstddef>
#include <cstdint>

// Eigene D3D11-Zeichenpfade fuer Shadow-Instancing (eigene Uebersetzungseinheit ohne CommonLib-PCH, da <d3d11.h>).
// Zeichnet gleiche Meshes der Sonnen-Schattenkarte mit einem Draw Call: eigener Vertex-Shader, der die Objektmatrix
// pro Instanz aus einem zweiten Vertex-Stream liest und dieselbe Kamera-Matrix (FrameBuffer b12, CameraViewProj)
// wie der Utility-Shader von Community Shaders verwendet.
namespace InstancedDraw
{
	// Objektmatrix (Zeilen 0-2 einer 4x4-Matrix, Zeile 3 = 0 0 0 1), kamerarelativ wie beim Utility-Shader
	struct Instance
	{
		float rows[3][4];
	};

	struct Group
	{
		void*         vertexBuffer;  // ID3D11Buffer*
		void*         indexBuffer;   // ID3D11Buffer*
		std::uint32_t stride;
		std::uint32_t indexCount;
		bool          fullPrecision;  // Position als 4x float32 statt 4x float16
		std::uint32_t firstInstance;  // Index in das Instanz-Array dieses Flushs
		std::uint32_t instanceCount;
	};

	// Einmalig (Main-Thread, Device vorhanden). Rueckgabe false + Fehlertext bei Problemen.
	bool Init(void* a_device, char* a_error, std::size_t a_errorSize) noexcept;
	bool Ready() noexcept;

	// Nach dem ersten normal gezeichneten Draw eines Batches: Pixel-Shader, Kamera-Constant-Buffer und
	// Rasterizer-/Depth-/Blend-Zustand merken, mit denen die Instanzen gezeichnet werden
	void CaptureState(void* a_context) noexcept;
	void ReleaseState() noexcept;

	// Diagnose: liest den aktuell im Vertex-Shader gebundenen Constant Buffer (Slot) aus (Stall, nur selten aufrufen).
	// Beruecksichtigt D3D11.1-Offsets. a_out erhaelt bis zu a_floats Floats ab dem Bindungs-Offset.
	bool DebugReadVSConstants(void* a_context, std::uint32_t a_slot, float* a_out, std::uint32_t a_floats) noexcept;

	// Zeichnet alle Gruppen; stellt den vorherigen D3D-Zustand danach wieder her.
	// a_clampZ: Variante RENDER_SHADOWMAP_CLAMPED (z = max(0, z))
	bool Flush(void* a_context, const Instance* a_instances, std::uint32_t a_instanceCount, const Group* a_groups, std::uint32_t a_groupCount, bool a_clampZ) noexcept;
}
