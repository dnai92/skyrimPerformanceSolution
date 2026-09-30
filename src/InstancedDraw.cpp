#include "InstancedDraw.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>

namespace InstancedDraw
{
	namespace
	{
		// Entspricht dem Shadowmap-Pfad von Utility.hlsl (Community Shaders):
		//   positionCS = mul(mul(CameraViewProj, World), float4(PositionMS.xyz, 1)), bei CLAMPED z = max(0, z)
		constexpr char kShaderSource[] = R"(
cbuffer PerFrame : register(b12)
{
	row_major float4x4 CameraView : packoffset(c0);
	row_major float4x4 CameraProj : packoffset(c4);
	row_major float4x4 CameraViewProj : packoffset(c8);
};

struct VS_INPUT
{
	float4 PositionMS : POSITION0;
	float4 World0 : TEXCOORD8;
	float4 World1 : TEXCOORD9;
	float4 World2 : TEXCOORD10;
};

float4 main(VS_INPUT input) : SV_POSITION
{
	float4x4 world = float4x4(input.World0, input.World1, input.World2, float4(0, 0, 0, 1));
	precise float4x4 modelViewProj = mul(CameraViewProj, world);
	precise float4 positionCS = mul(modelViewProj, float4(input.PositionMS.xyz, 1.0));
#ifdef CLAMP_Z
	positionCS.z = max(0, positionCS.z);
#endif
	return positionCS;
}
)";

		constexpr UINT kInstanceCapacity = 32768;  // Instanzen pro Frame (Ringpuffer)

		ID3D11Device*       g_device = nullptr;
		ID3D11VertexShader* g_vs[2]{};        // [0] normal, [1] CLAMP_Z
		ID3D11InputLayout*  g_layout[2]{};    // [0] Position float16x4, [1] float32x4
		ID3D11Buffer*       g_instanceVB = nullptr;
		UINT                g_ringPos = 0;
		bool                g_ready = false;

		// Zustand des ersten normal gezeichneten Draws (Referenzen gehalten)
		ID3D11PixelShader*      g_ps = nullptr;
		ID3D11Buffer*           g_cb12 = nullptr;
		ID3D11RasterizerState*  g_rs = nullptr;
		ID3D11DepthStencilState* g_ds = nullptr;
		UINT                    g_stencilRef = 0;
		ID3D11BlendState*       g_bs = nullptr;
		float                   g_blendFactor[4]{};
		UINT                    g_sampleMask = 0xFFFFFFFF;
		bool                    g_captured = false;

		template <class T>
		void SafeRelease(T*& a_p) noexcept
		{
			if (a_p) {
				a_p->Release();
				a_p = nullptr;
			}
		}

		using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
	}

	bool Ready() noexcept
	{
		return g_ready;
	}

	bool Init(void* a_device, char* a_error, std::size_t a_errorSize) noexcept
	{
		if (g_ready) {
			return true;
		}
		g_device = static_cast<ID3D11Device*>(a_device);
		if (!g_device) {
			std::snprintf(a_error, a_errorSize, "kein D3D11-Device");
			return false;
		}
		const auto compiler = LoadLibraryW(L"d3dcompiler_47.dll");
		const auto compile = compiler ? reinterpret_cast<D3DCompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
		if (!compile) {
			std::snprintf(a_error, a_errorSize, "d3dcompiler_47.dll/D3DCompile nicht gefunden");
			return false;
		}

		ID3DBlob* bytecode[2]{};
		for (int v = 0; v < 2; ++v) {
			const D3D_SHADER_MACRO defines[] = { { v ? "CLAMP_Z" : "NO_CLAMP", "1" }, { nullptr, nullptr } };
			ID3DBlob* errors = nullptr;
			const auto hr = compile(kShaderSource, sizeof(kShaderSource) - 1, "SkyrimPerfInstancedShadow", defines, nullptr, "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode[v], &errors);
			if (FAILED(hr)) {
				std::snprintf(a_error, a_errorSize, "Shader-Kompilierung fehlgeschlagen: %s", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
				SafeRelease(errors);
				return false;
			}
			SafeRelease(errors);
			if (FAILED(g_device->CreateVertexShader(bytecode[v]->GetBufferPointer(), bytecode[v]->GetBufferSize(), nullptr, &g_vs[v]))) {
				std::snprintf(a_error, a_errorSize, "CreateVertexShader fehlgeschlagen");
				return false;
			}
		}

		// Stream 0: Engine-Vertexpuffer (Position am Anfang des Vertex), Stream 1: Instanzmatrizen
		for (int p = 0; p < 2; ++p) {
			const D3D11_INPUT_ELEMENT_DESC elements[] = {
				{ "POSITION", 0, p ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 8, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
				{ "TEXCOORD", 9, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
				{ "TEXCOORD", 10, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
			};
			if (FAILED(g_device->CreateInputLayout(elements, 4, bytecode[0]->GetBufferPointer(), bytecode[0]->GetBufferSize(), &g_layout[p]))) {
				std::snprintf(a_error, a_errorSize, "CreateInputLayout fehlgeschlagen");
				return false;
			}
		}
		SafeRelease(bytecode[0]);
		SafeRelease(bytecode[1]);

		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = kInstanceCapacity * sizeof(Instance);
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(g_device->CreateBuffer(&desc, nullptr, &g_instanceVB))) {
			std::snprintf(a_error, a_errorSize, "CreateBuffer (Instanzen) fehlgeschlagen");
			return false;
		}
		g_ready = true;
		return true;
	}

	void ReleaseState() noexcept
	{
		SafeRelease(g_ps);
		SafeRelease(g_cb12);
		SafeRelease(g_rs);
		SafeRelease(g_ds);
		SafeRelease(g_bs);
		g_captured = false;
	}

	void CaptureState(void* a_context) noexcept
	{
		const auto ctx = static_cast<ID3D11DeviceContext*>(a_context);
		if (!ctx) {
			return;
		}
		ReleaseState();
		ctx->PSGetShader(&g_ps, nullptr, nullptr);
		ctx->VSGetConstantBuffers(12, 1, &g_cb12);
		ctx->RSGetState(&g_rs);
		ctx->OMGetDepthStencilState(&g_ds, &g_stencilRef);
		ctx->OMGetBlendState(&g_bs, g_blendFactor, &g_sampleMask);
		g_captured = true;
	}

	bool DebugReadVSConstants(void* a_context, std::uint32_t a_slot, float* a_out, std::uint32_t a_floats) noexcept
	{
		const auto ctx = static_cast<ID3D11DeviceContext*>(a_context);
		if (!ctx || !g_device) {
			return false;
		}
		ID3D11Buffer* cb = nullptr;
		UINT          first = 0, num = 0;
		ID3D11DeviceContext1* ctx1 = nullptr;
		if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1))) && ctx1) {
			ctx1->VSGetConstantBuffers1(a_slot, 1, &cb, &first, &num);
			ctx1->Release();
		} else {
			ctx->VSGetConstantBuffers(a_slot, 1, &cb);
		}
		if (!cb) {
			return false;
		}
		D3D11_BUFFER_DESC desc{};
		cb->GetDesc(&desc);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		desc.MiscFlags = 0;
		ID3D11Buffer* staging = nullptr;
		bool          ok = false;
		if (SUCCEEDED(g_device->CreateBuffer(&desc, nullptr, &staging))) {
			ctx->CopyResource(staging, cb);
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
				const UINT offsetBytes = first * 16;
				const UINT avail = desc.ByteWidth > offsetBytes ? (desc.ByteWidth - offsetBytes) / 4 : 0;
				const UINT n = a_floats < avail ? a_floats : avail;
				std::memcpy(a_out, static_cast<const char*>(mapped.pData) + offsetBytes, n * 4);
				ctx->Unmap(staging, 0);
				ok = n > 0;
			}
			staging->Release();
		}
		cb->Release();
		return ok;
	}

	bool Flush(void* a_context, const Instance* a_instances, std::uint32_t a_instanceCount, const Group* a_groups, std::uint32_t a_groupCount, bool a_clampZ) noexcept
	{
		const auto ctx = static_cast<ID3D11DeviceContext*>(a_context);
		if (!g_ready || !g_captured || !ctx || a_instanceCount == 0 || a_groupCount == 0 || a_instanceCount > kInstanceCapacity) {
			return false;
		}

		// Instanzdaten hochladen (Ringpuffer: NO_OVERWRITE solange Platz, sonst DISCARD)
		D3D11_MAP mapType = D3D11_MAP_WRITE_NO_OVERWRITE;
		if (g_ringPos + a_instanceCount > kInstanceCapacity || g_ringPos == 0) {
			mapType = D3D11_MAP_WRITE_DISCARD;
			g_ringPos = 0;
		}
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(ctx->Map(g_instanceVB, 0, mapType, 0, &mapped))) {
			return false;
		}
		std::memcpy(static_cast<char*>(mapped.pData) + g_ringPos * sizeof(Instance), a_instances, a_instanceCount * sizeof(Instance));
		ctx->Unmap(g_instanceVB, 0);
		const UINT base = g_ringPos;
		g_ringPos += a_instanceCount;

		// Vorherigen Zustand sichern
		ID3D11InputLayout*       oldLayout = nullptr;
		ID3D11Buffer*            oldVB[2]{};
		UINT                     oldStride[2]{}, oldOffset[2]{};
		ID3D11Buffer*            oldIB = nullptr;
		DXGI_FORMAT              oldIBFormat = DXGI_FORMAT_UNKNOWN;
		UINT                     oldIBOffset = 0;
		D3D11_PRIMITIVE_TOPOLOGY oldTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		ID3D11VertexShader*      oldVS = nullptr;
		ID3D11Buffer*            oldCB12 = nullptr;
		ID3D11PixelShader*       oldPS = nullptr;
		ID3D11RasterizerState*   oldRS = nullptr;
		ID3D11DepthStencilState* oldDS = nullptr;
		UINT                     oldStencilRef = 0;
		ID3D11BlendState*        oldBS = nullptr;
		float                    oldBlendFactor[4]{};
		UINT                     oldSampleMask = 0;
		ctx->IAGetInputLayout(&oldLayout);
		ctx->IAGetVertexBuffers(0, 2, oldVB, oldStride, oldOffset);
		ctx->IAGetIndexBuffer(&oldIB, &oldIBFormat, &oldIBOffset);
		ctx->IAGetPrimitiveTopology(&oldTopo);
		ctx->VSGetShader(&oldVS, nullptr, nullptr);
		ctx->VSGetConstantBuffers(12, 1, &oldCB12);
		ctx->PSGetShader(&oldPS, nullptr, nullptr);
		ctx->RSGetState(&oldRS);
		ctx->OMGetDepthStencilState(&oldDS, &oldStencilRef);
		ctx->OMGetBlendState(&oldBS, oldBlendFactor, &oldSampleMask);

		// Eigener Zustand
		ctx->VSSetShader(g_vs[a_clampZ ? 1 : 0], nullptr, 0);
		ctx->VSSetConstantBuffers(12, 1, &g_cb12);
		ctx->PSSetShader(g_ps, nullptr, 0);
		ctx->RSSetState(g_rs);
		ctx->OMSetDepthStencilState(g_ds, g_stencilRef);
		ctx->OMSetBlendState(g_bs, g_blendFactor, g_sampleMask);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		int currentLayout = -1;
		for (std::uint32_t i = 0; i < a_groupCount; ++i) {
			const auto& g = a_groups[i];
			const int   layout = g.fullPrecision ? 1 : 0;
			if (layout != currentLayout) {
				ctx->IASetInputLayout(g_layout[layout]);
				currentLayout = layout;
			}
			ID3D11Buffer* vbs[2] = { static_cast<ID3D11Buffer*>(g.vertexBuffer), g_instanceVB };
			const UINT    strides[2] = { g.stride, static_cast<UINT>(sizeof(Instance)) };
			const UINT    offsets[2] = { 0, 0 };
			ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
			ctx->IASetIndexBuffer(static_cast<ID3D11Buffer*>(g.indexBuffer), DXGI_FORMAT_R16_UINT, 0);
			ctx->DrawIndexedInstanced(g.indexCount, g.instanceCount, 0, 0, base + g.firstInstance);
		}

		// Zustand wiederherstellen
		ctx->IASetInputLayout(oldLayout);
		ctx->IASetVertexBuffers(0, 2, oldVB, oldStride, oldOffset);
		ctx->IASetIndexBuffer(oldIB, oldIBFormat, oldIBOffset);
		ctx->IASetPrimitiveTopology(oldTopo);
		ctx->VSSetShader(oldVS, nullptr, 0);
		ctx->VSSetConstantBuffers(12, 1, &oldCB12);
		ctx->PSSetShader(oldPS, nullptr, 0);
		ctx->RSSetState(oldRS);
		ctx->OMSetDepthStencilState(oldDS, oldStencilRef);
		ctx->OMSetBlendState(oldBS, oldBlendFactor, oldSampleMask);
		SafeRelease(oldLayout);
		SafeRelease(oldVB[0]);
		SafeRelease(oldVB[1]);
		SafeRelease(oldIB);
		SafeRelease(oldVS);
		SafeRelease(oldCB12);
		SafeRelease(oldPS);
		SafeRelease(oldRS);
		SafeRelease(oldDS);
		SafeRelease(oldBS);
		return true;
	}
}
