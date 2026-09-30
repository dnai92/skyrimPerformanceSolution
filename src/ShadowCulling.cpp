#include "ShadowCulling.h"

#include "Config.h"
#include "Stats.h"

namespace ShadowCulling
{
	namespace
	{
		constexpr std::size_t kMaxCascades = 4;

		// Vom Main-Thread pro Frame gesetzt, von den Culling-Jobs (Worker-Threads) gelesen
		// Zuordnung ueber die Kamera: parallele Culling-Jobs nutzen eigene Culler-Instanzen,
		// aber dieselbe Kaskaden-Kamera wie der Deskriptor.
		std::array<std::atomic<const RE::NiCamera*>, kMaxCascades> g_sunCameras{};
		std::array<RE::BSCullingProcess*, kMaxCascades>            g_descCullers{};  // nur Diagnose (Main-Thread)
		std::uint32_t                                              g_descCount = 0;  // nur Diagnose (Main-Thread)

		// Diagnose: welche Kameras tragen wie viele Meshes ein (lock-freie Tabelle)
		constexpr std::size_t                                      kDiagSlots = 32;
		std::array<std::atomic<const RE::NiCamera*>, kDiagSlots>   g_diagCameras{};
		std::array<std::atomic<std::uint32_t>, kDiagSlots>         g_diagCounts{};
		std::uint32_t                                              g_frameCounter = 0;

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
		std::atomic<float>                                           g_camX{ 0.0f };
		std::atomic<float>                                           g_camY{ 0.0f };
		std::atomic<float>                                           g_camZ{ 0.0f };

		int CascadeIndexOf(const RE::BSCullingProcess* a_culler) noexcept
		{
			const auto camera = a_culler->camera;
			if (!camera) {
				return -1;
			}
			for (std::size_t i = 0; i < kMaxCascades; ++i) {
				if (g_sunCameras[i].load(std::memory_order_relaxed) == camera) {
					return static_cast<int>(i);
				}
			}
			return -1;
		}

		bool ShouldCull(RE::BSGeometry& a_geom, std::uint32_t a_cascade) noexcept
		{
			const auto& cfg = Config::shadowCulling;
			if (!cfg.enabled || a_cascade < cfg.minCascade) {
				return false;
			}

			const auto& bound = a_geom.worldBound;
			if (bound.radius <= 0.0f || bound.radius >= cfg.maxRadius) {
				return false;
			}
			if (cfg.skipSkinned && a_geom.GetGeometryRuntimeData().skinInstance) {
				return false;
			}

			const float dx = bound.center.x - g_camX.load(std::memory_order_relaxed);
			const float dy = bound.center.y - g_camY.load(std::memory_order_relaxed);
			const float dz = bound.center.z - g_camZ.load(std::memory_order_relaxed);
			const float distance = std::sqrt(dx * dx + dy * dy + dz * dz) - bound.radius;
			if (distance <= cfg.minDistance) {
				return false;
			}
			return bound.radius / distance < cfg.minAngularSize;
		}

		struct AppendVirtual
		{
			static void thunk(RE::BSCullingProcess* a_this, RE::BSGeometry& a_visible, std::int32_t a_alphaGroupIndex)
			{
				DiagRecord(a_this->camera);
				const int cascade = CascadeIndexOf(a_this);
				if (cascade >= 0) {
					if (ShouldCull(a_visible, static_cast<std::uint32_t>(cascade))) {
						Stats::Count(Stats::Counter::SunCulled);
						return;
					}
					Stats::Count(cascade == 0 ? Stats::Counter::SunCascade0 : cascade == 1 ? Stats::Counter::SunCascade1 : Stats::Counter::SunCascade2Plus);
				} else {
					Stats::Count(Stats::Counter::OtherCullers);
				}
				func(a_this, a_visible, a_alphaGroupIndex);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_BSCullingProcess[0] };
		AppendVirtual::func = vtbl.write_vfunc(0x18, AppendVirtual::thunk);
		logger::info("Hook installiert: BSCullingProcess::AppendVirtual (vfunc 0x18)");
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
		g_descCullers = {};
		g_descCount = 0;
		if (const auto ssn = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0]) {
			if (const auto sun = ssn->GetRuntimeData().sunShadowDirLight) {
				const auto& descriptors = sun->GetRuntimeData().shadowmapDescriptors;
				g_descCount = descriptors.size();
				for (std::uint32_t i = 0; i < descriptors.size() && i < kMaxCascades; ++i) {
					cameras[i] = descriptors[i].camera.get();
					g_descCullers[i] = descriptors[i].cullingProcess;
				}
			}
		}
		for (std::size_t i = 0; i < kMaxCascades; ++i) {
			g_sunCameras[i].store(cameras[i], std::memory_order_relaxed);
		}

		// Diagnose alle ~600 Frames ins Log
		if (++g_frameCounter % 600 == 0) {
			logger::info("[ShadowCulling-Diag] Sonnen-Deskriptoren: {}", g_descCount);
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
