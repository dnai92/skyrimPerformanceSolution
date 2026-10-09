#include "OcclusionGpu.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace OcclusionGpu
{
	namespace
	{
		// Je Ausgabe-Kachel: kleinster und groesster Tiefenwert des zugehoerigen Bereichs im Quellpuffer.
		// Welcher davon "fern" ist, entscheidet die CPU anhand der Projektion (Standard- oder umgekehrtes Z).
		constexpr char kShaderSource[] = R"(
Texture2D<float> DepthTex : register(t0);
RWTexture2D<float2> OutTex : register(u0);
cbuffer Params : register(b0)
{
	uint SrcW;
	uint SrcH;
	uint DstW;
	uint DstH;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= DstW || id.y >= DstH)
		return;
	uint x0 = id.x * SrcW / DstW;
	uint x1 = max(x0 + 1, (id.x + 1) * SrcW / DstW);
	uint y0 = id.y * SrcH / DstH;
	uint y1 = max(y0 + 1, (id.y + 1) * SrcH / DstH);
	float lo = 1e30;
	float hi = -1e30;
	for (uint y = y0; y < y1; ++y) {
		for (uint x = x0; x < x1; ++x) {
			float d = DepthTex.Load(int3(x, y, 0));
			lo = min(lo, d);
			hi = max(hi, d);
		}
	}
	OutTex[id.xy] = float2(lo, hi);
}
)";

		struct Params
		{
			std::uint32_t srcW, srcH, dstW, dstH;
		};

		ID3D11Device*              g_device = nullptr;
		ID3D11ComputeShader*       g_cs = nullptr;
		ID3D11Buffer*              g_params = nullptr;
		ID3D11Texture2D*           g_target = nullptr;
		ID3D11UnorderedAccessView* g_uav = nullptr;
		ID3D11Texture2D*           g_staging[kRing]{};
		bool                       g_busy[kRing]{};
		std::uint32_t              g_tag[kRing]{};
		std::uint32_t              g_srcW[kRing]{}, g_srcH[kRing]{};
		std::uint32_t              g_write = 0, g_read = 0;  // naechster freier / aeltester belegter Platz
		bool                       g_ready = false;

		template <class T>
		void SafeRelease(T*& a_p) noexcept
		{
			if (a_p) {
				a_p->Release();
				a_p = nullptr;
			}
		}
	}

	bool Init(void* a_device, char* a_error, std::size_t a_errorSize) noexcept
	{
		if (g_ready) {
			return true;
		}
		g_device = static_cast<ID3D11Device*>(a_device);
		if (!g_device) {
			std::snprintf(a_error, a_errorSize, "no device");
			return false;
		}
		using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
		const auto compiler = LoadLibraryW(L"d3dcompiler_47.dll");
		const auto compile = compiler ? reinterpret_cast<D3DCompileFn>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
		if (!compile) {
			std::snprintf(a_error, a_errorSize, "d3dcompiler_47.dll/D3DCompile not found");
			return false;
		}
		ID3DBlob* bytecode = nullptr;
		ID3DBlob* errors = nullptr;
		const auto hr = compile(kShaderSource, sizeof(kShaderSource) - 1, "SPSOcclusionDownsample", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode, &errors);
		if (FAILED(hr) || !bytecode) {
			std::snprintf(a_error, a_errorSize, "compile failed 0x%08X: %s", static_cast<unsigned>(hr), errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
			SafeRelease(errors);
			return false;
		}
		SafeRelease(errors);
		const bool csOk = SUCCEEDED(g_device->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &g_cs));
		bytecode->Release();
		if (!csOk) {
			std::snprintf(a_error, a_errorSize, "CreateComputeShader failed");
			return false;
		}

		D3D11_BUFFER_DESC cb{};
		cb.ByteWidth = 16;
		cb.Usage = D3D11_USAGE_DYNAMIC;
		cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(g_device->CreateBuffer(&cb, nullptr, &g_params))) {
			std::snprintf(a_error, a_errorSize, "constant buffer failed");
			return false;
		}

		D3D11_TEXTURE2D_DESC td{};
		td.Width = kWidth;
		td.Height = kHeight;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R32G32_FLOAT;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		if (FAILED(g_device->CreateTexture2D(&td, nullptr, &g_target)) || FAILED(g_device->CreateUnorderedAccessView(g_target, nullptr, &g_uav))) {
			std::snprintf(a_error, a_errorSize, "target texture failed");
			return false;
		}
		td.Usage = D3D11_USAGE_STAGING;
		td.BindFlags = 0;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		for (auto& s : g_staging) {
			if (FAILED(g_device->CreateTexture2D(&td, nullptr, &s))) {
				std::snprintf(a_error, a_errorSize, "staging texture failed");
				return false;
			}
		}
		g_ready = true;
		return true;
	}

	bool Ready() noexcept { return g_ready; }

	bool Capture(void* a_context, void* a_depthSRV, std::uint32_t a_tag) noexcept
	{
		const auto ctx = static_cast<ID3D11DeviceContext*>(a_context);
		const auto srv = static_cast<ID3D11ShaderResourceView*>(a_depthSRV);
		if (!g_ready || !ctx || !srv || g_busy[g_write]) {
			return false;
		}
		ID3D11Resource* res = nullptr;
		srv->GetResource(&res);
		if (!res) {
			return false;
		}
		D3D11_RESOURCE_DIMENSION dim{};
		res->GetType(&dim);
		D3D11_TEXTURE2D_DESC sd{};
		if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
			static_cast<ID3D11Texture2D*>(res)->GetDesc(&sd);
		}
		res->Release();
		if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D || sd.SampleDesc.Count != 1 || sd.Width == 0 || sd.Height == 0) {
			return false;
		}

		D3D11_MAPPED_SUBRESOURCE m{};
		if (FAILED(ctx->Map(g_params, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
			return false;
		}
		const Params p{ sd.Width, sd.Height, kWidth, kHeight };
		std::memcpy(m.pData, &p, sizeof(p));
		ctx->Unmap(g_params, 0);

		// Compute-Zustand sichern (Community Shaders nutzt eigene Compute-Shader) und danach zuruecksetzen
		ID3D11ComputeShader*       oldCs = nullptr;
		ID3D11ClassInstance*       oldInst[256]{};
		UINT                       oldInstCount = 256;
		ID3D11ShaderResourceView*  oldSrv = nullptr;
		ID3D11UnorderedAccessView* oldUav = nullptr;
		ID3D11Buffer*              oldCb = nullptr;
		ctx->CSGetShader(&oldCs, oldInst, &oldInstCount);
		ctx->CSGetShaderResources(0, 1, &oldSrv);
		ctx->CSGetUnorderedAccessViews(0, 1, &oldUav);
		ctx->CSGetConstantBuffers(0, 1, &oldCb);

		ctx->CSSetShader(g_cs, nullptr, 0);
		ctx->CSSetShaderResources(0, 1, &srv);
		ctx->CSSetUnorderedAccessViews(0, 1, &g_uav, nullptr);
		ctx->CSSetConstantBuffers(0, 1, &g_params);
		ctx->Dispatch((kWidth + 7) / 8, (kHeight + 7) / 8, 1);

		ID3D11ShaderResourceView*  nullSrv = nullptr;
		ID3D11UnorderedAccessView* nullUav = nullptr;
		ctx->CSSetShaderResources(0, 1, oldSrv ? &oldSrv : &nullSrv);
		ctx->CSSetUnorderedAccessViews(0, 1, oldUav ? &oldUav : &nullUav, nullptr);
		ctx->CSSetConstantBuffers(0, 1, &oldCb);
		ctx->CSSetShader(oldCs, oldInstCount ? oldInst : nullptr, oldInstCount);
		SafeRelease(oldCs);
		for (UINT i = 0; i < oldInstCount; ++i) {
			SafeRelease(oldInst[i]);
		}
		SafeRelease(oldSrv);
		SafeRelease(oldUav);
		SafeRelease(oldCb);

		ctx->CopyResource(g_staging[g_write], g_target);
		g_busy[g_write] = true;
		g_tag[g_write] = a_tag;
		g_srcW[g_write] = sd.Width;
		g_srcH[g_write] = sd.Height;
		g_write = (g_write + 1) % kRing;
		return true;
	}

	bool Poll(void* a_context, Readback& a_out) noexcept
	{
		const auto ctx = static_cast<ID3D11DeviceContext*>(a_context);
		if (!g_ready || !ctx || !g_busy[g_read]) {
			return false;
		}
		D3D11_MAPPED_SUBRESOURCE m{};
		if (ctx->Map(g_staging[g_read], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) {
			return false;  // GPU noch nicht fertig (DXGI_ERROR_WAS_STILL_DRAWING)
		}
		a_out.minDepth.resize(kWidth * kHeight);
		a_out.maxDepth.resize(kWidth * kHeight);
		for (std::uint32_t y = 0; y < kHeight; ++y) {
			const auto row = reinterpret_cast<const float*>(static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch);
			for (std::uint32_t x = 0; x < kWidth; ++x) {
				a_out.minDepth[y * kWidth + x] = row[x * 2];
				a_out.maxDepth[y * kWidth + x] = row[x * 2 + 1];
			}
		}
		ctx->Unmap(g_staging[g_read], 0);
		a_out.tag = g_tag[g_read];
		a_out.srcW = g_srcW[g_read];
		a_out.srcH = g_srcH[g_read];
		g_busy[g_read] = false;
		g_read = (g_read + 1) % kRing;
		return true;
	}

	void Reset() noexcept
	{
		for (auto& b : g_busy) {
			b = false;
		}
		g_write = g_read = 0;
	}
}
