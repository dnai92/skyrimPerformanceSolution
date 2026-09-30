#include "ShadowCulling.h"

#include "Config.h"
#include "Stats.h"

namespace ShadowCulling
{
	namespace
	{
		constexpr std::size_t kMaxCascades = 4;

		// Vom Main-Thread pro Frame gesetzt, von den Culling-Jobs (Worker-Threads) gelesen
		std::array<std::atomic<RE::BSCullingProcess*>, kMaxCascades> g_sunCullers{};
		std::atomic<float>                                           g_camX{ 0.0f };
		std::atomic<float>                                           g_camY{ 0.0f };
		std::atomic<float>                                           g_camZ{ 0.0f };

		int CascadeIndexOf(const RE::BSCullingProcess* a_culler) noexcept
		{
			for (std::size_t i = 0; i < kMaxCascades; ++i) {
				if (g_sunCullers[i].load(std::memory_order_relaxed) == a_culler) {
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
		if (const auto camera = RE::PlayerCamera::GetSingleton(); camera && camera->cameraRoot) {
			const auto& pos = camera->cameraRoot->world.translate;
			g_camX.store(pos.x, std::memory_order_relaxed);
			g_camY.store(pos.y, std::memory_order_relaxed);
			g_camZ.store(pos.z, std::memory_order_relaxed);
		}

		std::array<RE::BSCullingProcess*, kMaxCascades> cullers{};
		if (const auto ssn = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0]) {
			if (const auto sun = ssn->GetRuntimeData().sunShadowDirLight) {
				const auto& descriptors = sun->GetRuntimeData().shadowmapDescriptors;
				for (std::uint32_t i = 0; i < descriptors.size() && i < kMaxCascades; ++i) {
					cullers[i] = descriptors[i].cullingProcess;
				}
			}
		}
		for (std::size_t i = 0; i < kMaxCascades; ++i) {
			g_sunCullers[i].store(cullers[i], std::memory_order_relaxed);
		}
	}
}
