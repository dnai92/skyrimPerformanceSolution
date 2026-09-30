#include "ShadowCulling.h"

#include "Config.h"
#include "Stats.h"

namespace ShadowCulling
{
	namespace
	{
		constexpr std::size_t kMaxCascades = 4;
		constexpr std::size_t kMaxPointCameras = 48;  // Schattenkarten-Kameras aller aktiven Punkt-/Spotlichter

		// Vom Main-Thread pro Frame gesetzt, von den Culling-Jobs (Worker-Threads) gelesen.
		// Zuordnung ueber die Kamera: parallele Culling-Jobs nutzen eigene Culler-Instanzen,
		// aber dieselbe Kamera wie der Schattenkarten-Deskriptor.
		std::array<std::atomic<const RE::NiCamera*>, kMaxCascades>     g_sunCameras{};
		std::array<std::atomic<const RE::NiCamera*>, kMaxPointCameras> g_pointCameras{};
		std::atomic<std::uint32_t>                                     g_pointCameraCount{ 0 };
		std::array<RE::BSCullingProcess*, kMaxCascades>                g_descCullers{};  // nur Diagnose (Main-Thread)
		std::uint32_t                                                  g_descCount = 0;  // nur Diagnose (Main-Thread)

		// Diagnose: welche Kameras tragen wie viele Meshes ein (lock-freie Tabelle)
		constexpr std::size_t                                    kDiagSlots = 32;
		std::array<std::atomic<const RE::NiCamera*>, kDiagSlots> g_diagCameras{};
		std::array<std::atomic<std::uint32_t>, kDiagSlots>       g_diagCounts{};
		std::uint32_t                                            g_frameCounter = 0;

		std::atomic<float> g_camX{ 0.0f };
		std::atomic<float> g_camY{ 0.0f };
		std::atomic<float> g_camZ{ 0.0f };

		void DiagRecord(const RE::NiCamera* a_camera) noexcept
		{
			for (std::size_t i = 0; i < kDiagSlots; ++i) {
				auto cur = g_diagCameras[i].load(std::memory_order_relaxed);
				if (cur == a_camera) {
					g_diagCounts[i].fetch_add(1, std::memory_order_relaxed);
					return;
				}
				if (!cur && g_diagCameras[i].compare_exchange_strong(cur, a_camera, std::memory_order_relaxed)) {
					g_diagCounts[i].fetch_add(1, std::memory_order_relaxed);
					return;
				}
				if (cur == a_camera) {
					g_diagCounts[i].fetch_add(1, std::memory_order_relaxed);
					return;
				}
			}
		}

		enum class Kind
		{
			kOther,
			kSun,
			kPoint
		};

		// Liefert die Art des Cullers; bei der Sonne zusaetzlich den Kaskaden-Index
		Kind Classify(const RE::BSCullingProcess* a_culler, std::uint32_t& a_cascade) noexcept
		{
			const auto camera = a_culler->camera;
			if (!camera) {
				return Kind::kOther;
			}
			for (std::uint32_t i = 0; i < kMaxCascades; ++i) {
				if (g_sunCameras[i].load(std::memory_order_relaxed) == camera) {
					a_cascade = i;
					return Kind::kSun;
				}
			}
			const auto count = g_pointCameraCount.load(std::memory_order_relaxed);
			for (std::uint32_t i = 0; i < count && i < kMaxPointCameras; ++i) {
				if (g_pointCameras[i].load(std::memory_order_relaxed) == camera) {
					return Kind::kPoint;
				}
			}
			return Kind::kOther;
		}

		bool ShouldCull(const Config::CullRule& a_rule, const RE::BSGeometry& a_geom, std::uint32_t a_cascade) noexcept
		{
			if (!a_rule.enabled || a_cascade < a_rule.minCascade) {
				return false;
			}

			const auto& bound = a_geom.worldBound;
			if (bound.radius <= 0.0f || bound.radius >= a_rule.maxRadius) {
				return false;
			}
			if (a_rule.skipSkinned && a_geom.GetGeometryRuntimeData().skinInstance) {
				return false;
			}

			const float dx = bound.center.x - g_camX.load(std::memory_order_relaxed);
			const float dy = bound.center.y - g_camY.load(std::memory_order_relaxed);
			const float dz = bound.center.z - g_camZ.load(std::memory_order_relaxed);
			const float distance = std::sqrt(dx * dx + dy * dy + dz * dz) - bound.radius;
			if (distance <= a_rule.minDistance) {
				return false;
			}
			return bound.radius / distance < a_rule.minAngularSize;
		}

		struct AppendVirtual
		{
			static void thunk(RE::BSCullingProcess* a_this, RE::BSGeometry& a_visible, std::int32_t a_alphaGroupIndex)
			{
				DiagRecord(a_this->camera);
				std::uint32_t cascade = 0;
				switch (Classify(a_this, cascade)) {
				case Kind::kSun:
					if (ShouldCull(Config::shadowCulling, a_visible, cascade)) {
						Stats::Count(Stats::Counter::SunCulled);
						return;
					}
					Stats::Count(cascade == 0 ? Stats::Counter::SunCascade0 : cascade == 1 ? Stats::Counter::SunCascade1 : Stats::Counter::SunCascade2Plus);
					break;
				case Kind::kPoint:
					// minCascade gilt nur fuer die Sonne -> hier Kaskade als "hinreichend gross" uebergeben
					if (ShouldCull(Config::pointLightCulling, a_visible, UINT32_MAX)) {
						Stats::Count(Stats::Counter::PointCulled);
						return;
					}
					Stats::Count(Stats::Counter::PointKept);
					break;
				default:
					Stats::Count(Stats::Counter::OtherCullers);
					break;
				}
				func(a_this, a_visible, a_alphaGroupIndex);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Markierung: Main::RenderDepth laeuft (Render-Thread = Main-Thread)
		std::atomic<bool> g_inDepthPrepass{ false };

		struct RenderDepth
		{
			static void thunk(bool a_arg1, bool a_arg2)
			{
				g_inDepthPrepass.store(true, std::memory_order_relaxed);
				func(a_arg1, a_arg2);
				g_inDepthPrepass.store(false, std::memory_order_relaxed);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Sucht in [a_begin, a_begin + a_size) nach einem call rel32 (E8) mit Ziel a_target
		std::uintptr_t FindCallTo(std::uintptr_t a_begin, std::size_t a_size, std::uintptr_t a_target) noexcept
		{
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_begin);
			for (std::size_t i = 0; i + 5 <= a_size; ++i) {
				if (bytes[i] != 0xE8) {
					continue;
				}
				std::int32_t rel;
				std::memcpy(&rel, bytes + i + 1, sizeof(rel));
				if (a_begin + i + 5 + static_cast<std::intptr_t>(rel) == a_target) {
					return a_begin + i;
				}
			}
			return 0;
		}

		// Eigene vtable von BSParabolicCullingProcess (Punktlicht-Schatten): der Eintrag 0x18 zeigt direkt auf die
		// Basis-Implementierung und laeuft daher NICHT ueber den Hook auf BSCullingProcess. Alles hier ist Punktlicht.
		struct AppendVirtualParabolic
		{
			static void thunk(RE::BSCullingProcess* a_this, RE::BSGeometry& a_visible, std::int32_t a_alphaGroupIndex)
			{
				DiagRecord(a_this->camera);
				if (ShouldCull(Config::pointLightCulling, a_visible, UINT32_MAX)) {
					Stats::Count(Stats::Counter::PointCulled);
					return;
				}
				Stats::Count(Stats::Counter::PointKept);
				func(a_this, a_visible, a_alphaGroupIndex);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// BSLightingShaderProperty::GetRenderPasses_Occlusion (vfunc 0x2D): liefert die Draws fuer die
		// Niederschlags-/Skylighting-Verdeckungskarte. Community Shaders ersetzt die Funktion (Objekte ab
		// Radius 32); wir haengen uns DAHINTER und werfen zusaetzlich kleinere Objekte hinaus.
		struct OcclusionRenderPasses
		{
			static RE::BSShaderProperty::RenderPassArray* thunk(RE::BSLightingShaderProperty* a_this, RE::BSGeometry* a_geometry, std::uint32_t a_renderMode, RE::BSShaderAccumulator* a_accumulator)
			{
				const auto passes = func(a_this, a_geometry, a_renderMode, a_accumulator);
				const auto& cfg = Config::skylightingCulling;
				if (passes && a_geometry && passes->head) {
					if (cfg.enabled && a_geometry->worldBound.radius < cfg.minRadius) {
						passes->Clear();
						Stats::Count(Stats::Counter::SkylightCulled);
					} else {
						Stats::Count(Stats::Counter::SkylightKept);
					}
				}
				return passes;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_BSCullingProcess[0] };
		AppendVirtual::func = vtbl.write_vfunc(0x18, AppendVirtual::thunk);
		logger::info("Hook installiert: BSCullingProcess::AppendVirtual (vfunc 0x18)");

		REL::Relocation<std::uintptr_t> parabolicVtbl{ RE::VTABLE_BSParabolicCullingProcess[0] };
		AppendVirtualParabolic::func = parabolicVtbl.write_vfunc(0x18, AppendVirtualParabolic::thunk);
		logger::info("Hook installiert: BSParabolicCullingProcess::AppendVirtual (vfunc 0x18)");
	}

	void InstallLate()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_BSLightingShaderProperty[0] };
		OcclusionRenderPasses::func = vtbl.write_vfunc(0x2D, OcclusionRenderPasses::thunk);
		logger::info("Hook installiert: BSLightingShaderProperty::GetRenderPasses_Occlusion (vfunc 0x2D, nach Community Shaders)");

		// Main::RenderDepth wird aus Main::RenderPlayerView per call aufgerufen. Beide Funktionen leitet Community Shaders
		// per Detours um (nur der Funktionsanfang) - daher die call-Stelle im Rumpf von RenderPlayerView suchen und dort einhaken.
		const auto renderDepth = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(100421, 107139) }.address();
		const auto playerView = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(35560, 36559) }.address();
		if (const auto site = FindCallTo(playerView, 0x1000, renderDepth)) {
			RenderDepth::func = SKSE::GetTrampoline().write_call<5>(site, RenderDepth::thunk);
			logger::info("Hook installiert: Main::RenderPlayerView -> Main::RenderDepth (call bei +0x{:X})", site - playerView);
		} else {
			logger::warn("Aufruf von Main::RenderDepth nicht gefunden - Tiefenvorpass-Culling deaktiviert");
		}
	}

	bool InDepthPrepass() noexcept { return g_inDepthPrepass.load(std::memory_order_relaxed); }

	bool ShouldSkipDepthPrepassDraw(const RE::BSRenderPass& a_pass) noexcept
	{
		if (!g_inDepthPrepass.load(std::memory_order_relaxed) || !a_pass.geometry) {
			return false;
		}
		if (ShouldCull(Config::depthPrepassCulling, *a_pass.geometry, UINT32_MAX)) {
			Stats::Count(Stats::Counter::DepthCulled);
			return true;
		}
		Stats::Count(Stats::Counter::DepthKept);
		return false;
	}

	void OnFrame()
	{
		Config::ReloadIfChanged();

		if (const auto camera = RE::PlayerCamera::GetSingleton(); camera && camera->cameraRoot) {
			const auto& pos = camera->cameraRoot->world.translate;
			g_camX.store(pos.x, std::memory_order_relaxed);
			g_camY.store(pos.y, std::memory_order_relaxed);
			g_camZ.store(pos.z, std::memory_order_relaxed);
		}

		std::array<const RE::NiCamera*, kMaxCascades> cameras{};
		std::uint32_t                                 pointCount = 0;
		g_descCullers = {};
		g_descCount = 0;
		if (const auto ssn = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0]) {
			auto&      ssnData = ssn->GetRuntimeData();
			const auto sun = ssnData.sunShadowDirLight;
			if (sun) {
				const auto& descriptors = sun->GetRuntimeData().shadowmapDescriptors;
				g_descCount = descriptors.size();
				for (std::uint32_t i = 0; i < descriptors.size() && i < kMaxCascades; ++i) {
					cameras[i] = descriptors[i].camera.get();
					g_descCullers[i] = descriptors[i].cullingProcess;
				}
			}
			for (const auto& light : ssnData.activeShadowLights) {
				if (!light || light.get() == sun) {
					continue;
				}
				for (const auto& desc : light->GetRuntimeData().shadowmapDescriptors) {
					if (pointCount < kMaxPointCameras && desc.camera) {
						g_pointCameras[pointCount++].store(desc.camera.get(), std::memory_order_relaxed);
					}
				}
			}
		}
		for (std::size_t i = 0; i < kMaxCascades; ++i) {
			g_sunCameras[i].store(cameras[i], std::memory_order_relaxed);
		}
		g_pointCameraCount.store(pointCount, std::memory_order_relaxed);

		// Diagnose alle ~600 Frames ins Log
		if (++g_frameCounter % 600 == 0) {
			logger::info("[ShadowCulling-Diag] Sonnen-Deskriptoren: {} | Punktlicht-Kameras: {}", g_descCount, pointCount);
			for (std::size_t i = 0; i < kMaxCascades && i < g_descCount; ++i) {
				logger::info("[ShadowCulling-Diag]   Kaskade {}: Kamera {:p}, Culler {:p}", i, static_cast<const void*>(cameras[i]), static_cast<const void*>(g_descCullers[i]));
			}
			for (std::size_t i = 0; i < kDiagSlots; ++i) {
				const auto cam = g_diagCameras[i].exchange(nullptr, std::memory_order_relaxed);
				const auto cnt = g_diagCounts[i].exchange(0, std::memory_order_relaxed);
				if (cnt > 0) {
					logger::info("[ShadowCulling-Diag]   Kamera {:p}: {} Meshes (~{}/Frame)", static_cast<const void*>(cam), cnt, cnt / 600);
				}
			}
		}
	}
}
