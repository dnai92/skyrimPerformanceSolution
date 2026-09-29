#include "RenderHooks.h"

#include "Stats.h"

namespace RenderHooks
{
	namespace
	{
		// ID3D11DeviceContext vtable-Indizes (d3d11.h)
		constexpr std::size_t kDrawIndexed = 12;
		constexpr std::size_t kDraw = 13;
		constexpr std::size_t kDrawIndexedInstanced = 20;
		constexpr std::size_t kDrawInstanced = 21;
		constexpr std::size_t kOMSetRenderTargets = 33;
		constexpr std::size_t kOMSetRenderTargetsAndUAVs = 34;
		constexpr std::size_t kDrawIndexedInstancedIndirect = 39;
		constexpr std::size_t kDrawInstancedIndirect = 40;
		constexpr std::size_t kDispatch = 41;

		constexpr std::uint32_t kKeepRenderTargets = 0xFFFFFFFF;  // D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL

		// Nur Aufrufe auf dem Immediate-Context des Spiels zaehlen (die vtable teilen sich ggf. auch
		// Contexts anderer Devices, z. B. DLSS/Streamline).
		void* g_gameCtx = nullptr;

		// Wird nur vom Render-Thread des Spiels (= Main-Thread) gesetzt/gelesen
		bool g_depthOnly = false;

		void OnDraw(void* a_ctx) noexcept
		{
			if (a_ctx == g_gameCtx) {
				Stats::Count(g_depthOnly ? Stats::Counter::DepthOnlyDraws : Stats::Counter::ColorDraws);
			}
		}

		bool IsDepthOnly(std::uint32_t a_numViews, void* const* a_rtvs, void* a_dsv) noexcept
		{
			if (!a_dsv) {
				return false;
			}
			for (std::uint32_t i = 0; i < a_numViews; ++i) {
				if (a_rtvs && a_rtvs[i]) {
					return false;
				}
			}
			return true;
		}

		template <std::size_t Index, class T>
		bool Patch(std::uintptr_t* a_vtbl, const char* a_name)
		{
			const auto original = a_vtbl[Index];
			const auto replacement = reinterpret_cast<std::uintptr_t>(&T::thunk);
			T::func = original;
			const bool ok = REL::safe_write(reinterpret_cast<std::uintptr_t>(&a_vtbl[Index]), &replacement, sizeof(replacement), &original, sizeof(original));
			logger::info("D3D11-Hook {}: {} (vtable[{}])", ok ? "installiert" : "FEHLGESCHLAGEN", a_name, Index);
			return ok;
		}

		struct DrawIndexed
		{
			static void thunk(void* a_ctx, std::uint32_t a_indexCount, std::uint32_t a_startIndex, std::int32_t a_baseVertex)
			{
				OnDraw(a_ctx);
				reinterpret_cast<decltype(&thunk)>(func)(a_ctx, a_indexCount, a_startIndex, a_baseVertex);
			}
			static inline std::uintptr_t func;
		};

		struct Draw
		{
			static void thunk(void* a_ctx, std::uint32_t a_vertexCount, std::uint32_t a_startVertex)
			{
				OnDraw(a_ctx);
				reinterpret_cast<decltype(&thunk)>(func)(a_ctx, a_vertexCount, a_startVertex);
			}
			static inline std::uintptr_t func;
		};

		struct DrawIndexedInstanced
		{
			static void thunk(void* a_ctx, std::uint32_t a_indexCount, std::uint32_t a_instanceCount, std::uint32_t a_startIndex, std::int32_t a_baseVertex, std::uint32_t a_startInstance)
			{
				OnDraw(a_ctx);
				reinterpret_cast<decltype(&thunk)>(func)(a_ctx, a_indexCount, a_instanceCount, a_startIndex, a_baseVertex, a_startInstance);
			}
			static inline std::uintptr_t func;
		};

		struct DrawInstanced
		{
			static void thunk(void* a_ctx, std::uint32_t a_vertexCount, std::uint32_t a_instanceCount, std::uint32_t a_startVertex, std::uint32_t a_startInstance)
			{
				OnDraw(a_ctx);
				reinterpret_cast<decltype(&thunk)>(func)(a_ctx, a_vertexCount, a_instanceCount, a_startVertex, a_startInstance);
			}
			static inline std::uintptr_t func;
		};

		struct DrawIndexedInstancedIndirect
		{
			static void thunk(void* a_ctx, void* a_args, std::uint32_t a_offset)
			{
				OnDraw(a_ctx);
				reinterpret_cast<decltype(&thunk)>(func)(a_ctx, a_args, a_offset);
			}
			static inline std::uintptr_t func;
		};

		struct DrawInstancedIndirect
		{
			static void thunk(void* a_ctx, void* a_args, std::uint32_t a_offset)
			{
				OnDraw(a_ctx);
				reinterpret_cast<decltype(&thunk)>(func)(a_ctx, a_args, a_offset);
			}
			static inline std::uintptr_t func;
		};

		struct Dispatch
		{
			static void thunk(void* a_ctx, std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_z)
			{
				if (a_ctx == g_gameCtx) {
					Stats::Count(Stats::Counter::Dispatches);
				}
				reinterpret_cast<decltype(&thunk)>(func)(a_ctx, a_x, a_y, a_z);
			}
			static inline std::uintptr_t func;
		};

		struct OMSetRenderTargets
		{
			static void thunk(void* a_ctx, std::uint32_t a_numViews, void* const* a_rtvs, void* a_dsv)
			{
				if (a_ctx == g_gameCtx) {
					g_depthOnly = IsDepthOnly(a_numViews, a_rtvs, a_dsv);
				}
				reinterpret_cast<decltype(&thunk)>(func)(a_ctx, a_numViews, a_rtvs, a_dsv);
			}
			static inline std::uintptr_t func;
		};

		struct OMSetRenderTargetsAndUAVs
		{
			static void thunk(void* a_ctx, std::uint32_t a_numRTVs, void* const* a_rtvs, void* a_dsv, std::uint32_t a_uavStart, std::uint32_t a_numUAVs, void* const* a_uavs, const std::uint32_t* a_uavCounts)
			{
				if (a_ctx == g_gameCtx && a_numRTVs != kKeepRenderTargets) {
					g_depthOnly = IsDepthOnly(a_numRTVs, a_rtvs, a_dsv);
				}
				reinterpret_cast<decltype(&thunk)>(func)(a_ctx, a_numRTVs, a_rtvs, a_dsv, a_uavStart, a_numUAVs, a_uavs, a_uavCounts);
			}
			static inline std::uintptr_t func;
		};
	}

	namespace
	{
		// Literale statt REX::W32-Konstanten: MEM_COMMIT/PAGE_NOACCESS sind ggf. als Windows-Makros definiert
		constexpr std::uint32_t kMemImage = 0x1000000;  // MEM_IMAGE
		constexpr std::uint32_t kMemCommit = 0x1000;    // MEM_COMMIT
		constexpr std::uint32_t kPageNoAccess = 0x01;   // PAGE_NOACCESS

		bool IsReadable(const void* a_ptr, std::size_t a_size) noexcept
		{
			REX::W32::MEMORY_BASIC_INFORMATION mbi{};
			if (!a_ptr || REX::W32::VirtualQuery(a_ptr, &mbi, sizeof(mbi)) == 0) {
				return false;
			}
			const auto end = static_cast<const std::byte*>(mbi.baseAddress) + mbi.regionSize;
			return mbi.state == kMemCommit && !(mbi.protect & kPageNoAccess) && static_cast<const std::byte*>(a_ptr) + a_size <= end;
		}

		bool IsInModuleImage(const void* a_ptr) noexcept
		{
			REX::W32::MEMORY_BASIC_INFORMATION mbi{};
			return a_ptr && REX::W32::VirtualQuery(a_ptr, &mbi, sizeof(mbi)) != 0 && mbi.type == kMemImage;
		}

		// Prueft, ob a_obj ein COM-Objekt mit dem Interface a_iid ist, BEVOR irgendeine virtuelle Methode
		// aufgerufen wird: Objekt lesbar, vtable und QueryInterface liegen in einem geladenen Modul.
		bool IsComObject(void* a_obj, const REX::W32::IID& a_iid, const char* a_what)
		{
			if (!IsReadable(a_obj, sizeof(void*))) {
				logger::warn("{} {:p}: nicht lesbar", a_what, a_obj);
				return false;
			}
			const auto vtbl = *static_cast<std::uintptr_t**>(a_obj);
			if (!IsInModuleImage(vtbl) || !IsReadable(vtbl, sizeof(std::uintptr_t) * (kDispatch + 1)) || !IsInModuleImage(reinterpret_cast<void*>(vtbl[0]))) {
				logger::warn("{} {:p}: vtable {:p} liegt in keinem Modul -> kein COM-Objekt", a_what, a_obj, static_cast<void*>(vtbl));
				return false;
			}
			void* out = nullptr;
			const auto unk = static_cast<REX::W32::IUnknown*>(a_obj);
			if (unk->QueryInterface(a_iid, &out) < 0 || !out) {
				logger::warn("{} {:p}: QueryInterface abgelehnt", a_what, a_obj);
				return false;
			}
			static_cast<REX::W32::IUnknown*>(out)->Release();
			return true;
		}

		REX::W32::ID3D11DeviceContext* FindGameContext()
		{
			// 1) Kontext aus dem Renderer-Singleton (RendererData ab Renderer+0x10)
			if (const auto renderer = RE::BSGraphics::Renderer::GetSingleton()) {
				const auto ctx = renderer->GetRuntimeData().context;
				if (IsComObject(ctx, REX::W32::IID_ID3D11DeviceContext, "Renderer.context")) {
					logger::info("D3D11-Context aus Renderer-Singleton");
					return ctx;
				}
			}
			// 2) Immediate-Context ueber das Device
			const auto device = RE::BSGraphics::Renderer::GetDevice();
			if (IsComObject(device, REX::W32::IID_ID3D11Device, "Device")) {
				REX::W32::ID3D11DeviceContext* ctx = nullptr;
				device->GetImmediateContext(&ctx);
				if (ctx) {
					ctx->Release();  // GetImmediateContext zaehlt hoch; das Device haelt den Context am Leben
					if (IsComObject(ctx, REX::W32::IID_ID3D11DeviceContext, "Device.ImmediateContext")) {
						logger::info("D3D11-Context ueber Device::GetImmediateContext");
						return ctx;
					}
				}
			}
			return nullptr;
		}
	}

	void Install()
	{
		const auto ctx = FindGameContext();
		if (!ctx) {
			logger::warn("Kein gueltiger D3D11-Context gefunden - Draw-Call-Zaehlung deaktiviert, nichts gepatcht");
			return;
		}

		g_gameCtx = ctx;
		auto vtbl = *reinterpret_cast<std::uintptr_t**>(ctx);
		logger::info("D3D11-Context {:p}, vtable {:p}", static_cast<void*>(ctx), static_cast<void*>(vtbl));

		// Render-Target-State zuerst, damit die Draw-Zuordnung von Anfang an stimmt
		Patch<kOMSetRenderTargets, OMSetRenderTargets>(vtbl, "OMSetRenderTargets");
		Patch<kOMSetRenderTargetsAndUAVs, OMSetRenderTargetsAndUAVs>(vtbl, "OMSetRenderTargetsAndUnorderedAccessViews");
		Patch<kDrawIndexed, DrawIndexed>(vtbl, "DrawIndexed");
		Patch<kDraw, Draw>(vtbl, "Draw");
		Patch<kDrawIndexedInstanced, DrawIndexedInstanced>(vtbl, "DrawIndexedInstanced");
		Patch<kDrawInstanced, DrawInstanced>(vtbl, "DrawInstanced");
		Patch<kDrawIndexedInstancedIndirect, DrawIndexedInstancedIndirect>(vtbl, "DrawIndexedInstancedIndirect");
		Patch<kDrawInstancedIndirect, DrawInstancedIndirect>(vtbl, "DrawInstancedIndirect");
		Patch<kDispatch, Dispatch>(vtbl, "Dispatch");
	}
}
