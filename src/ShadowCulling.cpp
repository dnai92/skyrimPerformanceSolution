#include "ShadowCulling.h"

#include "Config.h"
#include "DetourHelper.h"
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
		std::atomic<const RE::NiCamera*>                               g_mainCamera{ nullptr };
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

		// Sonnenhoehe als Sinus (1 = Zenit, 0 = Horizont). Bei tiefer Sonne werfen auch kleine Objekte lange
		// Schatten -> Sonnen-Culling bewertet dann die Schattenlaenge (Radius / sin) statt des Radius.
		std::atomic<float> g_sunSin{ 1.0f };
		constexpr float    kMinSunSin = 0.05f;
		std::atomic<bool>  g_sunCullAllowed{ true };  // false, solange die Sonne unter fMinSunElevation steht

		// Kaskaden-Cache (nur Main-Thread schreibt; Culling-Jobs lesen g_cacheSkip)
		std::atomic<bool> g_cacheSkip{ false };  // dieser Frame: ferne Kaskade(n) nicht neu zeichnen
		std::uint32_t     g_cacheCounter = 0;
		struct CachedCascade
		{
			bool                           valid = false;
			REX::W32::XMFLOAT4X4           lightTransform{};
			RE::RENDER_TARGET_DEPTHSTENCIL renderTarget{};
			std::uint32_t                  shadowmapIndex = 0;
			bool                           clearOriginal = true;
		};
		std::array<CachedCascade, kMaxCascades> g_cached{};
		std::uint32_t                           g_cacheInvalidations = 0;  // Diagnose: Ziel/Slice hat sich geaendert

		// Die Engine loescht jede Kaskaden-Ebene per ClearDepthStencilView, unabhaengig von clearRenderTarget
		// (RenderDoc). Deshalb wird die ferne Kaskade nach dem Zeichnen in eine eigene Textur kopiert und im
		// Cache-Frame nach dem (leeren) Render zurueckkopiert. Betroffen: Sonnen-Schattenkarte und die
		// Volumetric-Lighting-Schattenkarte von Community Shaders (beide 2 Ebenen = 2 Kaskaden).
		struct CascadeBackup
		{
			RE::RENDER_TARGETS_DEPTHSTENCIL::RENDER_TARGET_DEPTHSTENCIL target;
			REX::W32::ID3D11Texture2D*                                  source = nullptr;  // Engine-Textur (nicht besessen)
			REX::W32::ID3D11Texture2D*                                  copy = nullptr;    // eigene Kopie
			std::uint32_t                                               mipLevels = 1;
			std::uint32_t                                               arraySize = 0;
			bool                                                        allSlices = false;  // true: alle Ebenen sichern (nicht nur ab iCascade)
		};
		// Volumetric Lighting: beide Ebenen komplett aus dem Cache-Frame - im Test 0.9.1 flackerte sonst die
		// Helligkeit der ganzen Szene (Lichtstrahlen/Nebel werden offenbar aus der fernen Kaskade gespeist)
		std::array<CascadeBackup, 2> g_backups{ { { RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM }, { RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM, nullptr, nullptr, 1, 0, true } } };
		bool                         g_backupValid = false;  // Kopie passt zum letzten gezeichneten Frame
		bool                         g_backupFailed = false;

		// Kopie anlegen bzw. bei geaenderter Engine-Textur (Aufloesung) neu anlegen
		bool EnsureBackup(CascadeBackup& a_b, REX::W32::ID3D11Texture2D* a_src) noexcept
		{
			if (a_b.source == a_src && a_b.copy) {
				return true;
			}
			if (a_b.copy) {
				a_b.copy->Release();
				a_b.copy = nullptr;
			}
			a_b.source = a_src;
			if (!a_src) {
				return false;
			}
			REX::W32::D3D11_TEXTURE2D_DESC desc{};
			a_src->GetDesc(&desc);
			a_b.mipLevels = desc.mipLevels;
			a_b.arraySize = desc.arraySize;
			desc.usage = REX::W32::D3D11_USAGE_DEFAULT;
			desc.bindFlags = 0;
			desc.cpuAccessFlags = 0;
			desc.miscFlags = 0;
			const auto device = RE::BSGraphics::Renderer::GetDevice();
			if (!device || device->CreateTexture2D(&desc, nullptr, &a_b.copy) < 0 || !a_b.copy) {
				a_b.copy = nullptr;
				return false;
			}
			logger::info("Kaskaden-Cache: Sicherungstextur {}x{} x{} angelegt (Ziel {})", desc.width, desc.height, desc.arraySize, static_cast<std::uint32_t>(a_b.target));
			return true;
		}

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
			kPoint,
			kMain
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

		bool ShouldCull(const Config::CullRule& a_rule, const RE::BSGeometry& a_geom, std::uint32_t a_cascade, float a_radiusScale = 1.0f) noexcept
		{
			if (!a_rule.enabled || a_cascade < a_rule.minCascade || !Config::masterEnabled.load(std::memory_order_relaxed)) {
				return false;
			}

			const auto& bound = a_geom.worldBound;
			const float radius = bound.radius * a_radiusScale;  // Sonne: ungefaehre Schattenlaenge
			if (bound.radius <= 0.0f || radius >= a_rule.maxRadius) {
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
			return radius / distance < a_rule.minAngularSize;
		}

		float DistanceToCamera(const RE::NiBound& a_bound) noexcept
		{
			const float dx = a_bound.center.x - g_camX.load(std::memory_order_relaxed);
			const float dy = a_bound.center.y - g_camY.load(std::memory_order_relaxed);
			const float dz = a_bound.center.z - g_camZ.load(std::memory_order_relaxed);
			return std::sqrt(dx * dx + dy * dy + dz * dz) - a_bound.radius;
		}

		// Diagnose: Verteilung der Decals nach Entfernung (Zeilen) und Radius (Spalten)
		constexpr std::array<float, 5>                     kDecalDistEdges{ 500.0f, 1000.0f, 1500.0f, 3000.0f, 6000.0f };
		constexpr std::array<float, 5>                     kDecalRadEdges{ 25.0f, 50.0f, 100.0f, 200.0f, 500.0f };
		std::array<std::array<std::atomic<std::uint32_t>, 6>, 6> g_decalHist{};
		std::atomic<std::uint32_t>                          g_decalNames[4]{};  // nur Zaehler fuer Namen 'Decal', 'DecalDirt', sonstige, leer

		template <std::size_t N>
		std::size_t Bucket(const std::array<float, N>& a_edges, float a_value) noexcept
		{
			std::size_t i = 0;
			while (i < N && a_value >= a_edges[i]) {
				++i;
			}
			return i;
		}

		// Hauptszene: Decal-Culling (Laub, Schmutz, Fussabdruecke, Blut) und optionales Mikro-Culling winziger Objekte.
		// Die Hauptkamera ('WorldRoot Camera') laeuft NICHT ueber AppendVirtual -> Aufruf aus GetRenderPasses (vfunc 0x2A).
		bool ShouldCullMain(RE::BSGeometry& a_geom) noexcept
		{
			const auto  property = a_geom.GetGeometryRuntimeData().shaderProperty.get();
			const bool  isDecal = property && property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kDecal, RE::BSShaderProperty::EShaderPropertyFlag::kDynamicDecal);
			const auto& dec = Config::decalCulling;
			if (isDecal) {
				g_decalHist[Bucket(kDecalDistEdges, DistanceToCamera(a_geom.worldBound))][Bucket(kDecalRadEdges, a_geom.worldBound.radius)].fetch_add(1, std::memory_order_relaxed);
				const std::string_view name{ a_geom.name.c_str() ? a_geom.name.c_str() : "" };
				g_decalNames[name == "Decal" ? 0 : name == "DecalDirt" ? 1 : name.empty() ? 3 : 2].fetch_add(1, std::memory_order_relaxed);
				if (dec.enabled && Config::masterEnabled.load(std::memory_order_relaxed) && a_geom.worldBound.radius < dec.maxRadius && DistanceToCamera(a_geom.worldBound) > dec.maxDistance) {
					Stats::Count(Stats::Counter::DecalCulled);
					return true;
				}
				Stats::Count(Stats::Counter::DecalKept);
			}
			if (ShouldCull(Config::mainViewCulling, a_geom, UINT32_MAX)) {
				Stats::Count(Stats::Counter::MainCulled);
				return true;
			}
			Stats::Count(Stats::Counter::MainKept);
			return false;
		}

		struct AppendVirtual
		{
			static void thunk(RE::BSCullingProcess* a_this, RE::BSGeometry& a_visible, std::int32_t a_alphaGroupIndex)
			{
				DiagRecord(a_this->camera);
				std::uint32_t cascade = 0;
				switch (Classify(a_this, cascade)) {
				case Kind::kSun:
					if (g_cacheSkip.load(std::memory_order_relaxed) && cascade >= Config::cascadeCache.cascade) {
						Stats::Count(Stats::Counter::CascadeSkipped);
						return;  // Kaskade kommt diesen Frame aus dem Cache
					}
					if (g_sunCullAllowed.load(std::memory_order_relaxed) && ShouldCull(Config::shadowCulling, a_visible, cascade, 1.0f / g_sunSin.load(std::memory_order_relaxed))) {
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
			static inline void (*func)(bool, bool) = nullptr;
		};

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

		// BSLightingShaderProperty::GetRenderPasses (vfunc 0x2A): Draws fuer die normale Darstellung (Hauptszene,
		// Reflexions-Cubemaps). Community Shaders (TruePBR) haengt sich ebenfalls hier ein und ruft das Original auf;
		// wir liegen dahinter und leeren die Liste fuer ferne kleine Decals bzw. winzige Objekte.
		struct LightingRenderPasses
		{
			static RE::BSShaderProperty::RenderPassArray* thunk(RE::BSLightingShaderProperty* a_this, RE::BSGeometry* a_geometry, std::uint32_t a_renderFlags, RE::BSShaderAccumulator* a_accumulator)
			{
				const auto passes = func(a_this, a_geometry, a_renderFlags, a_accumulator);
				if (passes && passes->head && a_geometry && ShouldCullMain(*a_geometry)) {
					passes->Clear();
				}
				return passes;
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
					if (cfg.enabled && Config::masterEnabled.load(std::memory_order_relaxed) && a_geometry->worldBound.radius < cfg.minRadius) {
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
		LightingRenderPasses::func = vtbl.write_vfunc(0x2A, LightingRenderPasses::thunk);
		logger::info("Hook installiert: BSLightingShaderProperty::GetRenderPasses (vfunc 0x2A, nach Community Shaders)");
		logger::info("Hook installiert: BSLightingShaderProperty::GetRenderPasses_Occlusion (vfunc 0x2D, nach Community Shaders)");

		// Main::RenderDepth wird nur indirekt aufgerufen (kein direkter call/jmp im Spielcode). Community Shaders leitet den
		// Funktionsanfang per Detours um; Detours verkettet einen weiteren Hook sauber dahinter.
		RenderDepth::func = reinterpret_cast<void (*)(bool, bool)>(REL::Relocation<std::uintptr_t>{ RELOCATION_ID(100421, 107139) }.address());
		if (const auto err = DetourHelper::Attach(reinterpret_cast<void**>(&RenderDepth::func), reinterpret_cast<void*>(&RenderDepth::thunk)); err == 0) {
			logger::info("Hook installiert: Main::RenderDepth (Detours)");
		} else {
			logger::warn("Main::RenderDepth: Detours-Fehler {} - Tiefenvorpass-Culling deaktiviert", err);
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

	void BeforeSunAccumulate(RE::BSShadowDirectionalLight* a_light) noexcept
	{
		const auto& cfg = Config::cascadeCache;
		bool        skip = false;
		if (cfg.enabled && Config::masterEnabled.load(std::memory_order_relaxed) && cfg.interval > 1 && a_light) {
			const auto& descs = a_light->GetRuntimeData().shadowmapDescriptors;
			skip = (++g_cacheCounter % cfg.interval) != 0 && cfg.cascade < descs.size();
			// Nur ueberspringen, wenn eine Sicherung und fuer alle betroffenen Kaskaden ein gueltiger Stand vorliegt
			skip = skip && g_backupValid && !g_backupFailed;
			for (std::uint32_t i = cfg.cascade; skip && i < descs.size() && i < kMaxCascades; ++i) {
				skip = g_cached[i].valid;
			}
		} else {
			for (auto& c : g_cached) {
				c.valid = false;
			}
			g_backupValid = false;
		}
		g_cacheSkip.store(skip, std::memory_order_relaxed);
	}

	void AfterSunRender() noexcept
	{
		const auto& cfg = Config::cascadeCache;
		if (!cfg.enabled || !Config::masterEnabled.load(std::memory_order_relaxed) || cfg.interval <= 1 || g_backupFailed) {
			return;
		}
		const auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer) {
			return;
		}
		const auto context = renderer->GetRuntimeData().context;
		auto&      depth = renderer->GetDepthStencilData().depthStencils;
		if (!context) {
			return;
		}
		const bool skip = g_cacheSkip.load(std::memory_order_relaxed);
		bool       ok = true;
		for (auto& b : g_backups) {
			const auto src = depth[b.target].texture;
			if (!src) {
				continue;  // Volumetric-Schatten ggf. nicht vorhanden
			}
			if (!EnsureBackup(b, src)) {
				ok = false;
				continue;
			}
			for (std::uint32_t slice = b.allSlices ? 0 : cfg.cascade; slice < b.arraySize; ++slice) {
				const auto sub = slice * b.mipLevels;  // Mip 0 der Ebene
				if (skip) {
					context->CopySubresourceRegion(src, sub, 0, 0, 0, b.copy, sub, nullptr);  // alten Stand zuruecklegen
				} else {
					context->CopySubresourceRegion(b.copy, sub, 0, 0, 0, src, sub, nullptr);  // frisch gezeichneten Stand sichern
				}
			}
		}
		if (!ok) {
			g_backupFailed = true;
			g_backupValid = false;
			logger::error("Kaskaden-Cache: Sicherungstextur konnte nicht angelegt werden - Cache deaktiviert");
			return;
		}
		if (!skip) {
			g_backupValid = true;
		}
	}

	void AfterSunAccumulate(RE::BSShadowDirectionalLight* a_light) noexcept
	{
		if (!a_light) {
			return;
		}
		const auto& cfg = Config::cascadeCache;
		const bool  skip = g_cacheSkip.load(std::memory_order_relaxed);
		auto&       descs = a_light->GetRuntimeData().shadowmapDescriptors;
		for (std::uint32_t i = cfg.cascade; i < descs.size() && i < kMaxCascades; ++i) {
			auto& d = descs[i];
			auto& c = g_cached[i];
			if (skip) {
				if (d.renderTarget != c.renderTarget || d.shadowmapIndex != c.shadowmapIndex) {
					// Ziel hat sich geaendert -> alter Inhalt unbrauchbar
					++g_cacheInvalidations;
					c.valid = false;
					continue;
				}
				d.lightTransform = c.lightTransform;  // alte Matrix passend zum alten Inhalt der Schattenkarte
				d.clearRenderTarget = false;          // alten Inhalt NICHT loeschen
				Stats::Count(Stats::Counter::CascadeSkipFrames);
			} else {
				if (!c.valid) {
					c.clearOriginal = d.clearRenderTarget;
				}
				d.clearRenderTarget = c.clearOriginal;
				c.lightTransform = d.lightTransform;
				c.renderTarget = d.renderTarget;
				c.shadowmapIndex = d.shadowmapIndex;
				c.valid = true;
			}
		}
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

		g_mainCamera.store(RE::Main::WorldRootCamera(), std::memory_order_relaxed);

		std::array<const RE::NiCamera*, kMaxCascades> cameras{};
		std::uint32_t                                 pointCount = 0;
		g_descCullers = {};
		g_descCount = 0;
		if (const auto ssn = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0]) {
			auto&      ssnData = ssn->GetRuntimeData();
			const auto sun = ssnData.sunShadowDirLight;
			if (sun) {
				const auto& v = sun->GetShadowDirectionalLightRuntimeData().sunVector;
				const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
				const float sunSin = len > 0.0f ? std::clamp(std::abs(v.z) / len, kMinSunSin, 1.0f) : 1.0f;
				g_sunSin.store(sunSin, std::memory_order_relaxed);
				g_sunCullAllowed.store(sunSin >= std::sin(Config::sunMinElevation.load(std::memory_order_relaxed) * 0.0174533f), std::memory_order_relaxed);
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
			logger::info("[ShadowCulling-Diag] Sonnen-Deskriptoren: {} | Punktlicht-Kameras: {} | Sonnenhoehe sin={:.2f} (~{:.0f} Grad, Schattenfaktor {:.1f})",
				g_descCount, pointCount, g_sunSin.load(), std::asin(g_sunSin.load()) * 57.2958f, 1.0f / g_sunSin.load());
			// Kaskaden-Cache: Ziel-Textur und Slice aller Schattenkarten (Sonne + Punktlichter) - teilen sie sich etwas?
			if (const auto ssn = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0]) {
				auto& ssnData = ssn->GetRuntimeData();
				if (const auto sun = ssnData.sunShadowDirLight) {
					const auto& descs = sun->GetRuntimeData().shadowmapDescriptors;
					for (std::uint32_t i = 0; i < descs.size(); ++i) {
						const auto& d = descs[i];
						logger::info("[Cascade-Diag]   Sonne K{}: Ziel {} Slice {} Clear {} Aktiv {} Port {}x{}", i, static_cast<std::uint32_t>(d.renderTarget), d.shadowmapIndex,
							d.clearRenderTarget, d.isEnabled, d.port.GetWidth(), d.port.GetHeight());
					}
				}
				std::uint32_t n = 0;
				for (const auto& light : ssnData.activeShadowLights) {
					if (!light || light.get() == ssnData.sunShadowDirLight || n >= 6) {
						continue;
					}
					for (const auto& d : light->GetRuntimeData().shadowmapDescriptors) {
						logger::info("[Cascade-Diag]   Licht {}: Ziel {} Slice {}", n, static_cast<std::uint32_t>(d.renderTarget), d.shadowmapIndex);
					}
					++n;
				}
			}
			logger::info("[Cascade-Diag]   Cache-Invalidierungen (Ziel/Slice gewechselt): {}", g_cacheInvalidations);
			if (const auto mainCam = RE::Main::WorldRootCamera()) {
				logger::info("[ShadowCulling-Diag]   Hauptkamera (WorldRootCamera): {:p} '{}'", static_cast<const void*>(mainCam), mainCam->name.c_str());
			}
			if (const auto pc = RE::PlayerCamera::GetSingleton(); pc && pc->cameraRoot) {
				for (const auto& child : pc->cameraRoot->GetChildren()) {
					if (child && child->GetRTTI() && std::string_view{ child->GetRTTI()->GetName() } == "NiCamera") {
						logger::info("[ShadowCulling-Diag]   PlayerCamera-Kind (NiCamera): {:p} '{}'", static_cast<const void*>(child.get()), child->name.c_str());
					}
				}
			}
			for (std::size_t i = 0; i < kMaxCascades && i < g_descCount; ++i) {
				logger::info("[ShadowCulling-Diag]   Kaskade {}: Kamera {:p}, Culler {:p}", i, static_cast<const void*>(cameras[i]), static_cast<const void*>(g_descCullers[i]));
			}
			logger::info("[Decal-Diag] Decals/Frame nach Entfernung (Zeilen) x Radius (Spalten <25 <50 <100 <200 <500 >=500) | Namen: Decal {} DecalDirt {} sonstige {} leer {}",
				g_decalNames[0].exchange(0) / 600, g_decalNames[1].exchange(0) / 600, g_decalNames[2].exchange(0) / 600, g_decalNames[3].exchange(0) / 600);
			constexpr std::array<const char*, 6> kDistLabels{ "   <500", "  <1000", "  <1500", "  <3000", "  <6000", "  >6000" };
			for (std::size_t d = 0; d < 6; ++d) {
				std::string row;
				for (std::size_t rr = 0; rr < 6; ++rr) {
					row += std::format("{:7}", g_decalHist[d][rr].exchange(0, std::memory_order_relaxed) / 600);
				}
				logger::info("[Decal-Diag] {} {}", kDistLabels[d], row);
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
