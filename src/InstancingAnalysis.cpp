#include "InstancingAnalysis.h"

#include <mutex>
#include <unordered_map>

namespace InstancingAnalysis
{
	namespace
	{
		constexpr std::uint32_t kReportFrames = 600;

		// Wird waehrend BSShadowDirectionalLight::Render gesetzt (Render-Thread = Main-Thread)
		std::atomic<bool> g_inSunShadows{ false };

		// Ueber welche Aufrufstelle von RenderPassImmediately laeuft der aktuelle Draw? (0 = keine gehookte)
		thread_local std::uint32_t t_callSite = 0;

		struct Key
		{
			const void*   vertexBuffer;
			const void*   indexBuffer;
			std::uint32_t technique;  // BSRenderPass::passEnum (enthaelt u. a. Alpha-Test-Bits)

			bool operator==(const Key&) const = default;
		};

		struct KeyHash
		{
			std::size_t operator()(const Key& k) const noexcept
			{
				auto h = std::hash<const void*>{}(k.vertexBuffer);
				h ^= std::hash<const void*>{}(k.indexBuffer) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
				h ^= std::hash<std::uint32_t>{}(k.technique) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
				return h;
			}
		};

		struct GroupInfo
		{
			std::uint32_t count = 0;
			const char*   sampleName = nullptr;
		};

		constexpr std::size_t kCallSites = 3;  // 0 = unbekannt, 1 = RenderBatches, 2 = zweite Aufrufstelle

		// Pro Frame (nur Render-Thread, Mutex zur Sicherheit)
		std::mutex                                  g_lock;
		std::unordered_map<Key, GroupInfo, KeyHash> g_frameGroups;
		std::uint32_t                               g_framePasses = 0;
		std::uint32_t                               g_frameSkinned = 0;
		std::uint32_t                               g_frameNonTriShape = 0;
		std::uint32_t                               g_frameNoRendererData = 0;

		struct Window
		{
			std::uint64_t frames = 0;
			std::uint64_t passes = 0;
			std::uint64_t skinned = 0;
			std::uint64_t nonTriShape = 0;
			std::uint64_t noRendererData = 0;
			std::uint64_t groups = 0;
			std::uint64_t drawsIfInstanced2 = 0;
			std::uint64_t drawsIfInstanced4 = 0;
			std::uint64_t drawsIfInstanced8 = 0;
			std::uint64_t callSites[kCallSites]{};
			std::uint64_t typeCounts[12]{};
		} g_window;

		std::vector<std::pair<std::uint32_t, std::string>> g_topGroups;

		struct SunShadowRender
		{
			static void thunk(RE::BSShadowDirectionalLight* a_this, std::uint32_t& a_index)
			{
				g_inSunShadows.store(true, std::memory_order_relaxed);
				func(a_this, a_index);
				g_inSunShadows.store(false, std::memory_order_relaxed);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		template <std::uint32_t Site>
		struct RenderPassImmediately
		{
			static void thunk(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags)
			{
				const auto prev = t_callSite;
				t_callSite = Site;
				func(a_pass, a_technique, a_alphaTest, a_renderFlags);
				t_callSite = prev;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		void Record(RE::BSRenderPass& a_pass) noexcept
		{
			auto&      geom = *a_pass.geometry;
			auto&      data = geom.GetGeometryRuntimeData();
			const auto type = static_cast<std::uint32_t>(geom.GetType().get());

			std::scoped_lock lock{ g_lock };
			++g_framePasses;
			++g_window.callSites[t_callSite < kCallSites ? t_callSite : 0];
			if (type < std::size(g_window.typeCounts)) {
				++g_window.typeCounts[type];
			}
			if (data.skinInstance) {
				++g_frameSkinned;
				return;
			}
			if (type != static_cast<std::uint32_t>(RE::BSGeometry::Type::kTriShape)) {
				++g_frameNonTriShape;
				return;
			}
			const auto rd = data.rendererData;
			if (!rd || !rd->vertexBuffer || !rd->indexBuffer) {
				++g_frameNoRendererData;
				return;
			}
			try {
				auto& g = g_frameGroups[Key{ rd->vertexBuffer, rd->indexBuffer, a_pass.passEnum }];
				if (g.count++ == 0) {
					g.sampleName = geom.name.c_str();
				}
			} catch (...) {
			}
		}

		// BSUtilityShader::SetupGeometry (vfunc 0x6): wird fuer JEDEN Utility-Draw aufgerufen, egal ueber welchen Pfad
		struct UtilitySetupGeometry
		{
			static void thunk(RE::BSShader* a_this, RE::BSRenderPass* a_pass, std::uint32_t a_flags)
			{
				if (g_inSunShadows.load(std::memory_order_relaxed) && a_pass && a_pass->geometry) {
					Record(*a_pass);
				}
				func(a_this, a_pass, a_flags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		void EndFrame()
		{
			std::scoped_lock lock{ g_lock };
			if (g_framePasses == 0) {
				return;
			}
			auto& w = g_window;
			++w.frames;
			w.passes += g_framePasses;
			w.skinned += g_frameSkinned;
			w.nonTriShape += g_frameNonTriShape;
			w.noRendererData += g_frameNoRendererData;
			w.groups += g_frameGroups.size();

			const std::uint64_t notGroupable = g_frameSkinned + g_frameNonTriShape + g_frameNoRendererData;
			std::uint64_t       d2 = notGroupable, d4 = notGroupable, d8 = notGroupable;
			for (const auto& [key, g] : g_frameGroups) {
				d2 += g.count >= 2 ? 1 : g.count;
				d4 += g.count >= 4 ? 1 : g.count;
				d8 += g.count >= 8 ? 1 : g.count;
			}
			w.drawsIfInstanced2 += d2;
			w.drawsIfInstanced4 += d4;
			w.drawsIfInstanced8 += d8;

			if (w.frames % kReportFrames == kReportFrames - 1) {
				g_topGroups.clear();
				for (const auto& [key, g] : g_frameGroups) {
					g_topGroups.emplace_back(g.count, std::format("{} (Technik {:X})", g.sampleName ? g.sampleName : "?", key.technique));
				}
				std::ranges::sort(g_topGroups, std::greater{}, &std::pair<std::uint32_t, std::string>::first);
				if (g_topGroups.size() > 20) {
					g_topGroups.resize(20);
				}
			}

			g_frameGroups.clear();
			g_framePasses = g_frameSkinned = g_frameNonTriShape = g_frameNoRendererData = 0;
		}

		void Report()
		{
			std::scoped_lock lock{ g_lock };
			auto&            w = g_window;
			if (w.frames == 0) {
				return;
			}
			const auto f = static_cast<double>(w.frames);
			logger::info("[Instancing-Analyse] {} Frames | Sonnenschatten-Draws/Frame {:.0f} | geskinnt {:.0f} | kein TriShape {:.0f} | ohne Buffer {:.0f}",
				w.frames, w.passes / f, w.skinned / f, w.nonTriShape / f, w.noRendererData / f);
			logger::info("[Instancing-Analyse]   Aufrufstelle: RenderBatches {:.0f} | zweite Stelle {:.0f} | unbekannt {:.0f}",
				w.callSites[1] / f, w.callSites[2] / f, w.callSites[0] / f);
			logger::info("[Instancing-Analyse]   Mesh-Gruppen/Frame {:.0f} | Draws mit Instancing ab 2: {:.0f} ({:.0f}%) | ab 4: {:.0f} ({:.0f}%) | ab 8: {:.0f} ({:.0f}%)",
				w.groups / f,
				w.drawsIfInstanced2 / f, 100.0 * w.drawsIfInstanced2 / w.passes,
				w.drawsIfInstanced4 / f, 100.0 * w.drawsIfInstanced4 / w.passes,
				w.drawsIfInstanced8 / f, 100.0 * w.drawsIfInstanced8 / w.passes);
			std::string types;
			for (std::size_t i = 0; i < std::size(w.typeCounts); ++i) {
				if (w.typeCounts[i]) {
					types += std::format(" [{}]={:.0f}", i, w.typeCounts[i] / f);
				}
			}
			logger::info("[Instancing-Analyse]   Geometrie-Typen/Frame:{}", types);
			for (const auto& [count, name] : g_topGroups) {
				logger::info("[Instancing-Analyse]     {:5}x  {}", count, name);
			}
			w = {};
		}

		std::uint32_t g_frameCounter = 0;
	}

	void Install()
	{
		REL::Relocation<std::uintptr_t> sunVtbl{ RE::VTABLE_BSShadowDirectionalLight[0] };
		SunShadowRender::func = sunVtbl.write_vfunc(0xA, SunShadowRender::thunk);
		logger::info("Hook installiert: BSShadowDirectionalLight::Render (vfunc 0xA)");

		REL::Relocation<std::uintptr_t> utilVtbl{ RE::VTABLE_BSUtilityShader[0] };
		UtilitySetupGeometry::func = utilVtbl.write_vfunc(0x6, UtilitySetupGeometry::thunk);
		logger::info("Hook installiert: BSUtilityShader::SetupGeometry (vfunc 0x6)");

		// Aufrufstellen von RenderPassImmediately (dieselben wie Community Shaders/LightLimitFix) - nur zur Zuordnung
		auto& trampoline = SKSE::GetTrampoline();
		REL::Relocation<std::uintptr_t> site1{ RELOCATION_ID(100852, 107642), REL::Relocate(0x29E, 0x28F) };
		RenderPassImmediately<1>::func = trampoline.write_call<5>(site1.address(), RenderPassImmediately<1>::thunk);
		REL::Relocation<std::uintptr_t> site2{ RELOCATION_ID(100877, 107667), REL::Relocate(0x1E5, 0xED) };
		RenderPassImmediately<2>::func = trampoline.write_call<5>(site2.address(), RenderPassImmediately<2>::thunk);
		logger::info("Hooks installiert: RenderPassImmediately-Aufrufstellen 1 und 2");
	}

	void OnFrame()
	{
		EndFrame();
		if (++g_frameCounter % kReportFrames == 0) {
			Report();
		}
	}
}
