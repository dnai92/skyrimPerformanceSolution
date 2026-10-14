#include "FarShadowGpu.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <cstdio>

namespace FarShadowGpu
{
	namespace
	{
		// Vollbild-Dreieck; der Pixel-Shader schreibt die gespeicherte Tiefe als SV_Depth, der Tiefentest der Engine
		// entscheidet je Texel, ob der gespeicherte (unbewegliche) oder der frisch gezeichnete (bewegliche) Wert gilt.
		constexpr char kVS[] = R"(
float4 main(uint id : SV_VertexID) : SV_Position
{
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.5, 1.0);
}
)";
		constexpr char kPS[] = R"(
Texture2D<float> Cached : register(t0);
float main(float4 pos : SV_Position) : SV_Depth
{
	return Cached.Load(int3(int2(pos.xy), 0));
}
)";

		struct Copy
		{
			ID3D11Texture2D*          tex = nullptr;
			ID3D11ShaderResourceView* srv = nullptr;
		};
		struct Target
		{
			ID3D11Texture2D* source = nullptr;  // Engine-Textur, zu der die Kopien passen (nicht besessen)
			UINT             width = 0, height = 0;
			DXGI_FORMAT      format = DXGI_FORMAT_UNKNOWN;
			Copy             slots[2];
		};

		ID3D11Device*            g_device = nullptr;
		ID3D11VertexShader*      g_vs = nullptr;
		ID3D11PixelShader*       g_ps = nullptr;
		ID3D11RasterizerState*   g_rs = nullptr;
		ID3D11DepthStencilState* g_ds = nullptr;
		D3D11_COMPARISON_FUNC    g_func = D3D11_COMPARISON_LESS_EQUAL;
		bool                     g_funcKnown = false;
		Target                   g_targets[2];
		bool                     g_ready = false;

		template <class T>
		void SafeRelease(T*& a_p) noexcept
		{
			if (a_p) {
				a_p->Release();
				a_p = nullptr;
			}
		}

		// Lesbares Format einer Tiefen-Textur (Kopie ist typlos wie das Original)
		DXGI_FORMAT ReadFormat(DXGI_FORMAT a_typeless) noexcept
		{
			switch (a_typeless) {
			case DXGI_FORMAT_R32_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT:
			case DXGI_FORMAT_R32_FLOAT:
				return DXGI_FORMAT_R32_FLOAT;
			case DXGI_FORMAT_R24G8_TYPELESS:
			case DXGI_FORMAT_D24_UNORM_S8_UINT:
				return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
			case DXGI_FORMAT_R16_TYPELESS:
			case DXGI_FORMAT_D16_UNORM:
			case DXGI_FORMAT_R16_UNORM:
				return DXGI_FORMAT_R16_UNORM;
			case DXGI_FORMAT_R32G8X24_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
				return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
			default:
				return DXGI_FORMAT_UNKNOWN;
			}
		}

		// Kopie braucht ein typloses Format, um als Shader-Ressource lesbar zu sein (Tiefenformate selbst nicht)
		DXGI_FORMAT StorageFormat(DXGI_FORMAT a_fmt) noexcept
		{
			switch (a_fmt) {
			case DXGI_FORMAT_D32_FLOAT:
			case DXGI_FORMAT_R32_FLOAT:
				return DXGI_FORMAT_R32_TYPELESS;
			case DXGI_FORMAT_D24_UNORM_S8_UINT:
				return DXGI_FORMAT_R24G8_TYPELESS;
			case DXGI_FORMAT_D16_UNORM:
			case DXGI_FORMAT_R16_UNORM:
				return DXGI_FORMAT_R16_TYPELESS;
			case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
				return DXGI_FORMAT_R32G8X24_TYPELESS;
			default:
				return a_fmt;
			}
		}

		void ReleaseTarget(Target& a_t) noexcept
		{
			for (auto& c : a_t.slots) {
				SafeRelease(c.srv);
				SafeRelease(c.tex);
			}
			a_t = {};
		}

		void CreateDepthState() noexcept
		{
			SafeRelease(g_ds);
			D3D11_DEPTH_STENCIL_DESC dd{};
			dd.DepthEnable = TRUE;
			dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
			dd.DepthFunc = g_func;
			dd.StencilEnable = FALSE;
			g_device->CreateDepthStencilState(&dd, &g_ds);
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
		const auto build = [&](const char* a_src, std::size_t a_len, const char* a_profile, ID3DBlob*& a_out) {
			ID3DBlob* errors = nullptr;
			const auto hr = compile(a_src, a_len, "SPSFarShadowMerge", nullptr, nullptr, "main", a_profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &a_out, &errors);
			if (FAILED(hr) || !a_out) {
				std::snprintf(a_error, a_errorSize, "compile %s failed 0x%08X: %s", a_profile, static_cast<unsigned>(hr), errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
				SafeRelease(errors);
				return false;
			}
			SafeRelease(errors);
			return true;
		};
		ID3DBlob* vs = nullptr;
		ID3DBlob* ps = nullptr;
		if (!build(kVS, sizeof(kVS) - 1, "vs_5_0", vs) || !build(kPS, sizeof(kPS) - 1, "ps_5_0", ps)) {
			SafeRelease(vs);
			return false;
		}
		const bool ok = SUCCEEDED(g_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs)) &&
		                SUCCEEDED(g_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_ps));
		vs->Release();
		ps->Release();
		if (!ok) {
			std::snprintf(a_error, a_errorSize, "shader creation failed");
			return false;
		}
		D3D11_RASTERIZER_DESC rd{};
		rd.FillMode = D3D11_FILL_SOLID;
		rd.CullMode = D3D11_CULL_NONE;
		rd.DepthClipEnable = TRUE;
		if (FAILED(g_device->CreateRasterizerState(&rd, &g_rs))) {
			std::snprintf(a_error, a_errorSize, "rasterizer state failed");
			return false;
		}
		CreateDepthState();
		if (!g_ds) {
			std::snprintf(a_error, a_errorSize, "depth state failed");
			return false;
		}
		g_ready = true;
		return true;
	}

	bool Ready() noexcept { return g_ready; }

	bool Ensure(std::uint32_t a_target, void* a_texture, bool& a_recreated) noexcept
	{
		a_recreated = false;
		if (!g_ready || a_target >= 2 || !a_texture) {
			return false;
		}
		auto&      t = g_targets[a_target];
		const auto src = static_cast<ID3D11Texture2D*>(a_texture);
		D3D11_TEXTURE2D_DESC desc{};
		src->GetDesc(&desc);
		if (t.source == src && t.width == desc.Width && t.height == desc.Height && t.format == desc.Format && t.slots[0].srv && t.slots[1].srv) {
			return true;
		}
		ReleaseTarget(t);
		a_recreated = true;
		const auto storage = StorageFormat(desc.Format);
		const auto read = ReadFormat(storage);
		if (read == DXGI_FORMAT_UNKNOWN || desc.SampleDesc.Count != 1) {
			return false;
		}
		D3D11_TEXTURE2D_DESC cd{};
		cd.Width = desc.Width;
		cd.Height = desc.Height;
		cd.MipLevels = 1;
		cd.ArraySize = 1;
		cd.Format = storage;
		cd.SampleDesc.Count = 1;
		cd.Usage = D3D11_USAGE_DEFAULT;
		cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
		sd.Format = read;
		sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		sd.Texture2D.MipLevels = 1;
		for (auto& c : t.slots) {
			if (FAILED(g_device->CreateTexture2D(&cd, nullptr, &c.tex)) || FAILED(g_device->CreateShaderResourceView(c.tex, &sd, &c.srv))) {
				ReleaseTarget(t);
				return false;
			}
		}
		t.source = src;
		t.width = desc.Width;
		t.height = desc.Height;
		t.format = desc.Format;
		return true;
	}

	void Save(void* a_context, std::uint32_t a_target, std::uint32_t a_slot, void* a_texture, std::uint32_t a_slice) noexcept
	{
		const auto ctx = static_cast<ID3D11DeviceContext*>(a_context);
		if (!ctx || a_target >= 2 || a_slot >= 2 || !g_targets[a_target].slots[a_slot].tex || !a_texture) {
			return;
		}
		D3D11_TEXTURE2D_DESC desc{};
		static_cast<ID3D11Texture2D*>(a_texture)->GetDesc(&desc);
		ctx->CopySubresourceRegion(g_targets[a_target].slots[a_slot].tex, 0, 0, 0, 0, static_cast<ID3D11Resource*>(a_texture), a_slice * desc.MipLevels, nullptr);
	}

	void Restore(void* a_context, std::uint32_t a_target, std::uint32_t a_slot, void* a_texture, std::uint32_t a_slice) noexcept
	{
		const auto ctx = static_cast<ID3D11DeviceContext*>(a_context);
		if (!ctx || a_target >= 2 || a_slot >= 2 || !g_targets[a_target].slots[a_slot].tex || !a_texture) {
			return;
		}
		D3D11_TEXTURE2D_DESC desc{};
		static_cast<ID3D11Texture2D*>(a_texture)->GetDesc(&desc);
		ctx->CopySubresourceRegion(static_cast<ID3D11Resource*>(a_texture), a_slice * desc.MipLevels, 0, 0, 0, g_targets[a_target].slots[a_slot].tex, 0, nullptr);
	}

	bool Merge(void* a_context, std::uint32_t a_target, std::uint32_t a_slot, void* a_dsv) noexcept
	{
		const auto ctx = static_cast<ID3D11DeviceContext*>(a_context);
		if (!g_ready || !ctx || !a_dsv || a_target >= 2 || a_slot >= 2) {
			return false;
		}
		const auto& t = g_targets[a_target];
		if (!t.slots[a_slot].srv) {
			return false;
		}
		// Zustand sichern
		ID3D11RenderTargetView*   oldRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView*   oldDsv = nullptr;
		ID3D11DepthStencilState*  oldDs = nullptr;
		UINT                      oldRef = 0;
		ID3D11RasterizerState*    oldRs = nullptr;
		D3D11_VIEWPORT            oldVp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		UINT                      vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		ID3D11InputLayout*        oldLayout = nullptr;
		D3D11_PRIMITIVE_TOPOLOGY  oldTopo{};
		ID3D11VertexShader*       oldVs = nullptr;
		ID3D11PixelShader*        oldPs = nullptr;
		ID3D11GeometryShader*     oldGs = nullptr;
		ID3D11HullShader*         oldHs = nullptr;
		ID3D11DomainShader*       oldDomain = nullptr;
		ID3D11ShaderResourceView* oldSrv = nullptr;
		ID3D11BlendState*         oldBlend = nullptr;
		FLOAT                     oldFactor[4]{};
		UINT                      oldMask = 0;
		ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, &oldDsv);
		ctx->OMGetDepthStencilState(&oldDs, &oldRef);
		ctx->OMGetBlendState(&oldBlend, oldFactor, &oldMask);
		ctx->RSGetState(&oldRs);
		ctx->RSGetViewports(&vpCount, oldVp);
		ctx->IAGetInputLayout(&oldLayout);
		ctx->IAGetPrimitiveTopology(&oldTopo);
		ctx->VSGetShader(&oldVs, nullptr, nullptr);
		ctx->PSGetShader(&oldPs, nullptr, nullptr);
		ctx->GSGetShader(&oldGs, nullptr, nullptr);
		ctx->HSGetShader(&oldHs, nullptr, nullptr);
		ctx->DSGetShader(&oldDomain, nullptr, nullptr);
		ctx->PSGetShaderResources(0, 1, &oldSrv);

		// Zeichnen
		ctx->OMSetRenderTargets(0, nullptr, static_cast<ID3D11DepthStencilView*>(a_dsv));
		ctx->OMSetDepthStencilState(g_ds, 0);
		ctx->RSSetState(g_rs);
		D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<FLOAT>(t.width), static_cast<FLOAT>(t.height), 0.0f, 1.0f };
		ctx->RSSetViewports(1, &vp);
		ctx->IASetInputLayout(nullptr);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ctx->VSSetShader(g_vs, nullptr, 0);
		ctx->PSSetShader(g_ps, nullptr, 0);
		ctx->GSSetShader(nullptr, nullptr, 0);
		ctx->HSSetShader(nullptr, nullptr, 0);
		ctx->DSSetShader(nullptr, nullptr, 0);
		ctx->PSSetShaderResources(0, 1, &t.slots[a_slot].srv);
		ctx->Draw(3, 0);

		// Zustand zurueck
		ID3D11ShaderResourceView* nullSrv = nullptr;
		ctx->PSSetShaderResources(0, 1, oldSrv ? &oldSrv : &nullSrv);
		ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, oldDsv);
		ctx->OMSetDepthStencilState(oldDs, oldRef);
		ctx->OMSetBlendState(oldBlend, oldFactor, oldMask);
		ctx->RSSetState(oldRs);
		ctx->RSSetViewports(vpCount, oldVp);
		ctx->IASetInputLayout(oldLayout);
		ctx->IASetPrimitiveTopology(oldTopo);
		ctx->VSSetShader(oldVs, nullptr, 0);
		ctx->PSSetShader(oldPs, nullptr, 0);
		ctx->GSSetShader(oldGs, nullptr, 0);
		ctx->HSSetShader(oldHs, nullptr, 0);
		ctx->DSSetShader(oldDomain, nullptr, 0);
		for (auto& r : oldRtv) {
			SafeRelease(r);
		}
		SafeRelease(oldDsv);
		SafeRelease(oldDs);
		SafeRelease(oldBlend);
		SafeRelease(oldRs);
		SafeRelease(oldLayout);
		SafeRelease(oldVs);
		SafeRelease(oldPs);
		SafeRelease(oldGs);
		SafeRelease(oldHs);
		SafeRelease(oldDomain);
		SafeRelease(oldSrv);
		return true;
	}

	bool DepthFuncKnown() noexcept { return g_funcKnown; }

	void NoteDepthState(void* a_context) noexcept
	{
		const auto ctx = static_cast<ID3D11DeviceContext*>(a_context);
		if (g_funcKnown || !ctx) {
			return;
		}
		ID3D11DepthStencilState* ds = nullptr;
		UINT                     ref = 0;
		ctx->OMGetDepthStencilState(&ds, &ref);
		if (!ds) {
			return;
		}
		D3D11_DEPTH_STENCIL_DESC d{};
		ds->GetDesc(&d);
		ds->Release();
		if (!d.DepthEnable || d.DepthFunc == D3D11_COMPARISON_ALWAYS || d.DepthFunc == D3D11_COMPARISON_NEVER) {
			return;  // kein aussagekraeftiger Zustand - beim naechsten Draw erneut
		}
		g_funcKnown = true;
		// Gleichstand zulassen (gespeicherter Wert = frisch gezeichneter Wert)
		g_func = d.DepthFunc == D3D11_COMPARISON_GREATER || d.DepthFunc == D3D11_COMPARISON_GREATER_EQUAL ? D3D11_COMPARISON_GREATER_EQUAL : D3D11_COMPARISON_LESS_EQUAL;
		if (g_ready) {
			CreateDepthState();
		}
	}

	void Reset() noexcept
	{
		for (auto& t : g_targets) {
			ReleaseTarget(t);
		}
	}
}
