#include "FarShadowGpu.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>

namespace FarShadowGpu
{
	namespace
	{
		struct Target
		{
			ID3D11Texture2D* source = nullptr;  // Engine-Textur, zu der die Kopien passen (nicht besessen)
			UINT             width = 0, height = 0;
			DXGI_FORMAT      format = DXGI_FORMAT_UNKNOWN;
			ID3D11Texture2D* slots[2]{};
		};
		Target g_target;

		void ReleaseTarget() noexcept
		{
			for (auto& c : g_target.slots) {
				if (c) {
					c->Release();
					c = nullptr;
				}
			}
			g_target = {};
		}
	}

	bool Ensure(void* a_texture, bool& a_recreated) noexcept
	{
		a_recreated = false;
		const auto src = static_cast<ID3D11Texture2D*>(a_texture);
		if (!src) {
			return false;
		}
		D3D11_TEXTURE2D_DESC desc{};
		src->GetDesc(&desc);
		auto& t = g_target;
		if (t.source == src && t.width == desc.Width && t.height == desc.Height && t.format == desc.Format && t.slots[0] && t.slots[1]) {
			return true;
		}
		ReleaseTarget();
		a_recreated = true;
		if (desc.SampleDesc.Count != 1) {
			return false;
		}
		ID3D11Device* device = nullptr;
		src->GetDevice(&device);
		if (!device) {
			return false;
		}
		// Reine Kopierziele (keine Bindung noetig): gleiches Format wie die Engine-Textur, eine Ebene
		D3D11_TEXTURE2D_DESC cd{};
		cd.Width = desc.Width;
		cd.Height = desc.Height;
		cd.MipLevels = 1;
		cd.ArraySize = 1;
		cd.Format = desc.Format;
		cd.SampleDesc.Count = 1;
		cd.Usage = D3D11_USAGE_DEFAULT;
		bool ok = true;
		for (auto& c : t.slots) {
			if (FAILED(device->CreateTexture2D(&cd, nullptr, &c)) || !c) {
				ok = false;
				break;
			}
		}
		device->Release();
		if (!ok) {
			ReleaseTarget();
			return false;
		}
		t.source = src;
		t.width = desc.Width;
		t.height = desc.Height;
		t.format = desc.Format;
		return true;
	}

	namespace
	{
		// Die Schattenkarte ist beim Kopieren als Tiefenpuffer gebunden. Kopieren aus/in eine gebundene Ressource ist in
		// D3D11 nicht erlaubt - NVIDIA nimmt es hin, AMD-Treiber koennen die Kopie verwerfen (Review 1.0.69). Wie beim
		// Licht-Cache die Ziele kurz abhaengen.
		struct Unbind
		{
			ID3D11DeviceContext*    ctx;
			ID3D11RenderTargetView* rtvs[8]{};
			ID3D11DepthStencilView* dsv = nullptr;

			explicit Unbind(ID3D11DeviceContext* a_ctx) noexcept :
				ctx(a_ctx)
			{
				ctx->OMGetRenderTargets(8, rtvs, &dsv);
				ctx->OMSetRenderTargets(0, nullptr, nullptr);
			}
			~Unbind()
			{
				ctx->OMSetRenderTargets(8, rtvs, dsv);
				for (auto& v : rtvs) {
					if (v) {
						v->Release();
					}
				}
				if (dsv) {
					dsv->Release();
				}
			}
		};
	}

	void Save(void* a_context, std::uint32_t a_slot, void* a_texture, std::uint32_t a_slice) noexcept
	{
		const auto ctx = static_cast<ID3D11DeviceContext*>(a_context);
		if (!ctx || a_slot >= 2 || !g_target.slots[a_slot] || !a_texture) {
			return;
		}
		D3D11_TEXTURE2D_DESC desc{};
		static_cast<ID3D11Texture2D*>(a_texture)->GetDesc(&desc);
		Unbind unbind(ctx);
		ctx->CopySubresourceRegion(g_target.slots[a_slot], 0, 0, 0, 0, static_cast<ID3D11Resource*>(a_texture), a_slice * desc.MipLevels, nullptr);
	}

	void Restore(void* a_context, std::uint32_t a_slot, void* a_texture, std::uint32_t a_slice) noexcept
	{
		const auto ctx = static_cast<ID3D11DeviceContext*>(a_context);
		if (!ctx || a_slot >= 2 || !g_target.slots[a_slot] || !a_texture) {
			return;
		}
		D3D11_TEXTURE2D_DESC desc{};
		static_cast<ID3D11Texture2D*>(a_texture)->GetDesc(&desc);
		Unbind unbind(ctx);
		ctx->CopySubresourceRegion(static_cast<ID3D11Resource*>(a_texture), a_slice * desc.MipLevels, 0, 0, 0, g_target.slots[a_slot], 0, nullptr);
	}

	bool BoundSlice(void* a_context, void* a_texture, std::uint32_t a_slice) noexcept
	{
		const auto ctx = static_cast<ID3D11DeviceContext*>(a_context);
		if (!ctx || !a_texture) {
			return false;
		}
		ID3D11DepthStencilView* dsv = nullptr;
		ctx->OMGetRenderTargets(0, nullptr, &dsv);
		if (!dsv) {
			return false;
		}
		ID3D11Resource* res = nullptr;
		dsv->GetResource(&res);
		D3D11_DEPTH_STENCIL_VIEW_DESC d{};
		dsv->GetDesc(&d);
		dsv->Release();
		if (res) {
			res->Release();  // die Engine haelt die Textur
		}
		return res == static_cast<ID3D11Resource*>(a_texture) && (d.Flags & D3D11_DSV_READ_ONLY_DEPTH) == 0 && d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY &&
		       d.Texture2DArray.FirstArraySlice == a_slice && d.Texture2DArray.ArraySize == 1;
	}

	void Reset() noexcept
	{
		ReleaseTarget();
	}
}
