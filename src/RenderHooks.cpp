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

	void Install()
	{
		const auto data = RE::BSGraphics::Renderer::GetRendererDataSingleton();
		const auto ctx = data ? data->context : nullptr;
		if (!ctx) {
			logger::warn("D3D11-Context nicht verfuegbar - Draw-Call-Zaehlung deaktiviert");
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
