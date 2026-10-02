#include "ShadowCulling.h"

#include "Config.h"
#include "DetourHelper.h"
#include "Stats.h"

#include <mutex>
#include <unordered_map>

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
			// Kamera der Kaskade: die Engine setzt daraus die Shader-Konstanten fuer das Abtasten der Schattenkarte.
			// Im Cache-Frame (leere Kaskade) passt sie Tiefenbereich/Ausschnitt an -> Sonne scheint durch Objekte.
			RE::NiTransform                camWorld{};
			float                          camWorldToCam[4][4]{};
			RE::NiCamera::RUNTIME_DATA2    camData2{};
			RE::NiFrustumPlanes            clipPlanes{};
			// Frisch berechneter Engine-Stand der Kamera im Cache-Frame: wird vor dem naechsten Accumulate
			// zurueckgeschrieben, damit UpdateCamera von ihrem eigenen Stand weiterrechnet (0.9.4 rechnete vom
			// eingefrorenen Stand weiter -> Schatten sprangen staerker)
			RE::NiTransform                engWorld{};
			float                          engWorldToCam[4][4]{};
			RE::NiCamera::RUNTIME_DATA2    engData2{};
			RE::NiFrustumPlanes            engClipPlanes{};
			bool                           camTouched = false;
			// lightTransform NACH Render (falls Render sie neu berechnet) - diese Matrix gehoert zum Inhalt der Karte
			REX::W32::XMFLOAT4X4           lightTransformFinal{};
			REX::W32::XMFLOAT4X4           lightTransformAfterAccum{};  // Diagnose
			bool                           finalValid = false;
			RE::NiRect<std::int32_t>       port{};
			bool                           isEnabled = true;
			bool                           clearSaved = false;    // clearRenderTarget vor unserer Aenderung (nur Cache-Frame)
			bool                           clearTouched = false;  // wir haben clearRenderTarget in diesem Frame geaendert
		};
		RE::BSShadowDirectionalLight* g_cacheLight = nullptr;      // Licht des laufenden Frames (Main-Thread)
		std::array<float, 3>          g_cachedStartSplits{};       // Kaskaden-Grenzen des letzten gezeichneten Frames
		std::array<float, 3>          g_cachedEndSplits{};

		// Diagnose: was rechnet die Engine im Cache-Frame (leere ferne Kaskade) anders als im letzten gezeichneten Frame?
		struct FrameDiff
		{
			std::uint32_t frames = 0;
			std::uint32_t lightTransform = 0, worldToCam = 0, frustum = 0, clipPlanes = 0, splits = 0, enabled = 0, port = 0;
			float         maxLightTransform = 0.0f, maxWorldToCam = 0.0f, maxFrustum = 0.0f, maxSplits = 0.0f;
		};
		FrameDiff g_diffCache{};   // Cache-Frame vs. letzter gezeichneter Frame
		FrameDiff g_diffNormal{};  // gezeichneter Frame vs. vorheriger gezeichneter Frame (Vergleichswert)
		// Projektions-Vergleich: Stand nach UpdateCamera im aktuellen Frame vs. im letzten gezeichneten Frame
		struct Projection
		{
			float          worldToCam[4][4]{};
			RE::NiFrustum  frustum{};
		};
		std::array<Projection, kMaxCascades> g_projCurrent{};
		std::array<Projection, kMaxCascades> g_projReference{};
		bool                                 g_projReferenceValid = false;
		bool                                 g_projSameThisFrame = false;
		bool                                 g_projSeenThisFrame = false;
		bool                                 g_inSunAccumulate = false;
		std::uint32_t                        g_projSame = 0, g_projChecked = 0;
		std::uint32_t g_renderChangedMatrix = 0, g_renderChecked = 0;  // Diagnose: aendert Render die lightTransform?
		float         g_renderChangedMax = 0.0f;

		float MaxAbsDiff(const float* a_a, const float* a_b, std::size_t a_n) noexcept
		{
			float m = 0.0f;
			for (std::size_t i = 0; i < a_n; ++i) {
				m = std::max(m, std::abs(a_a[i] - a_b[i]));
			}
			return m;
		}

		template <class Cached>
		void Compare(FrameDiff& a_out, const RE::BSShadowLight::ShadowmapDescriptor& a_d, const Cached& a_c, const RE::BSShadowDirectionalLight* a_light) noexcept
		{
			constexpr float eps = 1e-4f;
			++a_out.frames;
			const float lt = MaxAbsDiff(&a_d.lightTransform.m[0][0], &a_c.lightTransform.m[0][0], 16);
			if (lt > eps) {
				++a_out.lightTransform;
				a_out.maxLightTransform = std::max(a_out.maxLightTransform, lt);
			}
			if (const auto cam = a_d.camera.get()) {
				const float wc = MaxAbsDiff(&cam->GetRuntimeData().worldToCam[0][0], &a_c.camWorldToCam[0][0], 16);
				if (wc > eps) {
					++a_out.worldToCam;
					a_out.maxWorldToCam = std::max(a_out.maxWorldToCam, wc);
				}
				const auto& f1 = cam->GetRuntimeData2().viewFrustum;
				const auto& f2 = a_c.camData2.viewFrustum;
				const float fr = std::max({ std::abs(f1.fLeft - f2.fLeft), std::abs(f1.fRight - f2.fRight), std::abs(f1.fTop - f2.fTop),
					std::abs(f1.fBottom - f2.fBottom), std::abs(f1.fNear - f2.fNear), std::abs(f1.fFar - f2.fFar) });
				if (fr > eps) {
					++a_out.frustum;
					a_out.maxFrustum = std::max(a_out.maxFrustum, fr);
				}
			}
			if (std::memcmp(&a_d.clipPlanes, &a_c.clipPlanes, sizeof(a_d.clipPlanes)) != 0) {
				++a_out.clipPlanes;
			}
			if (a_d.isEnabled != a_c.isEnabled) {
				++a_out.enabled;
			}
			if (std::memcmp(&a_d.port, &a_c.port, sizeof(a_d.port)) != 0) {
				++a_out.port;
			}
			if (a_light) {
				const auto& dir = a_light->GetShadowDirectionalLightRuntimeData();
				const float sp = std::max(MaxAbsDiff(dir.startSplitDistances, g_cachedStartSplits.data(), 3), MaxAbsDiff(dir.endSplitDistances, g_cachedEndSplits.data(), 3));
				if (sp > eps) {
					++a_out.splits;
					a_out.maxSplits = std::max(a_out.maxSplits, sp);
				}
			}
		}

		void LogDiff(const char* a_label, FrameDiff& a_d)
		{
			logger::info("[Cascade-Diag]   {}: {} Frames | lightTransform {} (max {:.4f}) | worldToCam {} (max {:.4f}) | Frustum {} (max {:.2f}) | clipPlanes {} | Grenzen {} (max {:.1f}) | isEnabled {} | Port {}",
				a_label, a_d.frames, a_d.lightTransform, a_d.maxLightTransform, a_d.worldToCam, a_d.maxWorldToCam, a_d.frustum, a_d.maxFrustum, a_d.clipPlanes, a_d.splits, a_d.maxSplits, a_d.enabled, a_d.port);
			a_d = {};
		}
		std::array<std::uint32_t, 2>  g_clearFlagSeen{};           // Diagnose: Normal-Frames mit clearRenderTarget false/true (ferne Kaskade)
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
		bool BackupWanted(const CascadeBackup& a_b) noexcept
		{
			return a_b.allSlices ? Config::cascadeCache.restoreVolumetric : Config::cascadeCache.restoreShadowmap;
		}
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

		// Diagnose: welche GROSSEN Objekte verwirft das Sonnen-Culling? (Tor-Schatten fehlt in der Ferne)
		// Schluessel = Meshname; gemerkt wird je Name Anzahl, groesster Radius, naechste/fernste Entfernung, Kaskade.
		struct CulledInfo
		{
			std::uint32_t count = 0;
			float         maxRadius = 0.0f;
			float         minDist = 1e30f;
			float         maxDist = 0.0f;
			float         angular = 0.0f;  // Radius/Distanz des naechsten Treffers
			std::uint32_t cascade = 0;
		};
		std::mutex                                  g_culledLock;
		std::unordered_map<std::string, CulledInfo> g_culledBig;
		constexpr float                             kCulledDiagMinRadius = 60.0f;  // nur Objekte ab dieser Groesse (ca. 0,85 m)

		void RecordCulled(const RE::BSGeometry& a_geom, std::uint32_t a_cascade, float a_distance) noexcept
		{
			const float radius = a_geom.worldBound.radius;
			if (radius < kCulledDiagMinRadius) {
				return;
			}
			try {
				const char* name = a_geom.name.c_str();
				std::scoped_lock lock{ g_culledLock };
				auto& info = g_culledBig[name && *name ? name : "(ohne Name)"];
				++info.count;
				info.maxRadius = std::max(info.maxRadius, radius);
				if (a_distance < info.minDist) {
					info.minDist = a_distance;
					info.angular = a_distance > 0.0f ? radius / a_distance : 0.0f;
				}
				info.maxDist = std::max(info.maxDist, a_distance);
				info.cascade = a_cascade;
			} catch (...) {
			}
		}

		void ReportCulled()
		{
			std::vector<std::pair<std::string, CulledInfo>> list;
			{
				std::scoped_lock lock{ g_culledLock };
				list.assign(g_culledBig.begin(), g_culledBig.end());
				g_culledBig.clear();
			}
			std::ranges::sort(list, [](const auto& a, const auto& b) { return a.second.maxRadius > b.second.maxRadius; });
			logger::info("[Culled-Diag] Groesste verworfene Sonnenschatten-Objekte (Radius >= {:.0f}), ueber 600 Frames, {} verschiedene Namen:", kCulledDiagMinRadius, list.size());
			for (std::size_t i = 0; i < list.size() && i < 25; ++i) {
				const auto& [name, c] = list[i];
				logger::info("[Culled-Diag]   {:<40} Radius {:6.0f} | Distanz {:6.0f}-{:6.0f} | Radius/Distanz {:.3f} | Kaskade {} | {}x", name, c.maxRadius, c.minDist, c.maxDist, c.angular, c.cascade, c.count);
			}
		}

		// Entfernte Charaktere: geskinnte Meshes ab fMinDistance nicht in die Schattenkarte (true = verwerfen)
		bool CullActorShadow(const RE::BSGeometry& a_geom, bool a_pointLight) noexcept;

		float DistanceToCamera(const RE::NiBound& a_bound) noexcept
		{
			const float dx = a_bound.center.x - g_camX.load(std::memory_order_relaxed);
			const float dy = a_bound.center.y - g_camY.load(std::memory_order_relaxed);
			const float dz = a_bound.center.z - g_camZ.load(std::memory_order_relaxed);
			return std::sqrt(dx * dx + dy * dy + dz * dz) - a_bound.radius;
		}

		bool CullActorShadow(const RE::BSGeometry& a_geom, bool a_pointLight) noexcept
		{
			const auto& cfg = Config::actorShadowCulling;
			if (!cfg.enabled || (a_pointLight && !cfg.pointLights) || !Config::masterEnabled.load(std::memory_order_relaxed) ||
				!const_cast<RE::BSGeometry&>(a_geom).GetGeometryRuntimeData().skinInstance) {
				return false;
			}
			if (DistanceToCamera(a_geom.worldBound) > cfg.minDistance) {
				Stats::Count(Stats::Counter::ActorShadowCulled);
				return true;
			}
			Stats::Count(Stats::Counter::ActorShadowKept);
			return false;
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
					if (g_cacheSkip.load(std::memory_order_relaxed) && Config::cascadeCache.skipDraws && cascade >= Config::cascadeCache.cascade) {
						Stats::Count(Stats::Counter::CascadeSkipped);
						return;  // Kaskade kommt diesen Frame aus dem Cache
					}
					if (g_sunCullAllowed.load(std::memory_order_relaxed) && CullActorShadow(a_visible, false)) {
						return;
					}
					if (g_sunCullAllowed.load(std::memory_order_relaxed) && ShouldCull(Config::shadowCulling, a_visible, cascade, 1.0f / g_sunSin.load(std::memory_order_relaxed))) {
						Stats::Count(Stats::Counter::SunCulled);
						RecordCulled(a_visible, cascade, DistanceToCamera(a_visible.worldBound));
						return;
					}
					Stats::Count(cascade == 0 ? Stats::Counter::SunCascade0 : cascade == 1 ? Stats::Counter::SunCascade1 : Stats::Counter::SunCascade2Plus);
					break;
				case Kind::kPoint:
					if (CullActorShadow(a_visible, true)) {
						return;
					}
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

	void SetInSunAccumulate(bool a_in) noexcept
	{
		g_inSunAccumulate = a_in;
	}

	void AfterSunUpdateCamera(RE::BSShadowDirectionalLight* a_light) noexcept
	{
		if (!a_light) {
			return;
		}
		const auto& cfg = Config::cascadeCache;
		auto&       descs = a_light->GetRuntimeData().shadowmapDescriptors;
		bool        same = g_projReferenceValid;
		for (std::uint32_t i = cfg.cascade; i < descs.size() && i < kMaxCascades; ++i) {
			const auto cam = descs[i].camera.get();
			if (!cam) {
				same = false;
				continue;
			}
			auto& p = g_projCurrent[i];
			std::memcpy(p.worldToCam, cam->GetRuntimeData().worldToCam, sizeof(p.worldToCam));
			p.frustum = cam->GetRuntimeData2().viewFrustum;
			if (same) {
				const auto& r = g_projReference[i];
				const float d1 = MaxAbsDiff(&p.worldToCam[0][0], &r.worldToCam[0][0], 16);
				const float d2 = MaxAbsDiff(&p.frustum.fLeft, &r.frustum.fLeft, 6);
				same = d1 <= cfg.projectionEpsilon && d2 <= cfg.projectionEpsilon;
			}
		}
		g_projSameThisFrame = same;
		g_projSeenThisFrame = true;
		++g_projChecked;
		if (same) {
			++g_projSame;
		}
		// Laeuft UpdateCamera innerhalb von Accumulate (vor dem Culling), gilt die Entscheidung sofort
		if (g_inSunAccumulate && cfg.requireSameProjection && !same) {
			g_cacheSkip.store(false, std::memory_order_relaxed);
		}
	}

	void BeforeSunAccumulate(RE::BSShadowDirectionalLight* a_light) noexcept
	{
		const auto& cfg = Config::cascadeCache;
		// Kamera-Stand der Engine aus dem letzten Cache-Frame zuruecklegen (auch wenn der Cache inzwischen aus ist)
		if (a_light) {
			auto& descs = a_light->GetRuntimeData().shadowmapDescriptors;
			for (std::uint32_t i = 0; i < descs.size() && i < kMaxCascades; ++i) {
				auto& c = g_cached[i];
				if (!c.camTouched) {
					continue;
				}
				descs[i].clipPlanes = c.engClipPlanes;
				if (const auto cam = descs[i].camera.get()) {
					cam->world = c.engWorld;
					std::memcpy(cam->GetRuntimeData().worldToCam, c.engWorldToCam, sizeof(c.engWorldToCam));
					cam->GetRuntimeData2() = c.engData2;
				}
				c.camTouched = false;
			}
		}
		bool        skip = false;
		if (cfg.enabled && Config::masterEnabled.load(std::memory_order_relaxed) && cfg.interval > 1 && a_light) {
			const auto& descs = a_light->GetRuntimeData().shadowmapDescriptors;
			skip = (++g_cacheCounter % cfg.interval) != 0 && cfg.cascade < descs.size();
			// Nur ueberspringen, wenn eine Sicherung und fuer alle betroffenen Kaskaden ein gueltiger Stand vorliegt
			skip = skip && g_backupValid && !g_backupFailed;
			// UpdateCamera lief bereits vor Accumulate -> Ergebnis hier beruecksichtigen
			if (cfg.requireSameProjection && g_projSeenThisFrame && !g_projSameThisFrame) {
				skip = false;
			}
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
		// clearRenderTarget der Engine zuruecksetzen (auch wenn der Cache inzwischen abgeschaltet wurde)
		if (g_cacheLight) {
			auto&      descs = g_cacheLight->GetRuntimeData().shadowmapDescriptors;
			const bool skipNow = g_cacheSkip.load(std::memory_order_relaxed);
			for (std::uint32_t i = 0; i < descs.size() && i < kMaxCascades; ++i) {
				auto& c = g_cached[i];
				if (c.clearTouched) {
					descs[i].clearRenderTarget = c.clearSaved;
					c.clearTouched = false;
				}
				if (i < cfg.cascade) {
					continue;
				}
				// Diagnose: hat Render die Matrix nach Accumulate noch veraendert?
				const float diff = MaxAbsDiff(&descs[i].lightTransform.m[0][0], &c.lightTransformAfterAccum.m[0][0], 16);
				++g_renderChecked;
				if (diff > 1e-4f) {
					++g_renderChangedMatrix;
					g_renderChangedMax = std::max(g_renderChangedMax, diff);
				}
				if (skipNow && cfg.freezeMatrix && c.finalValid) {
					descs[i].lightTransform = c.lightTransformFinal;  // gilt fuer alle spaeteren Leser (CS Deferred)
				} else if (!skipNow) {
					c.lightTransformFinal = descs[i].lightTransform;
					c.finalValid = true;
				}
			}
			// Gezeichneter Frame: seine Projektion ist die Referenz fuer die naechsten Cache-Frames
			if (!skipNow && g_projSeenThisFrame) {
				g_projReference = g_projCurrent;
				g_projReferenceValid = true;
			}
			g_projSeenThisFrame = false;
		}
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
			if (!src || !BackupWanted(b)) {
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
		g_cacheLight = a_light;
		for (std::uint32_t i = cfg.cascade; i < descs.size() && i < kMaxCascades; ++i) {
			auto& d = descs[i];
			auto& c = g_cached[i];
			c.clearTouched = false;
			if (skip && cfg.freezeMatrix && c.finalValid) {
				c.lightTransformAfterAccum = c.lightTransformFinal;  // Vergleich nach Render gegen den eingefrorenen Wert
			} else {
				c.lightTransformAfterAccum = d.lightTransform;
			}
			if (skip) {
				if (d.renderTarget != c.renderTarget || d.shadowmapIndex != c.shadowmapIndex) {
					// Ziel hat sich geaendert -> alter Inhalt unbrauchbar
					++g_cacheInvalidations;
					c.valid = false;
					continue;
				}
				Compare(g_diffCache, d, c, a_light);
				if (cfg.freezeMatrix) {
					d.lightTransform = c.finalValid ? c.lightTransformFinal : c.lightTransform;  // alte Matrix passend zum alten Inhalt
				}
				if (cfg.freezeCamera) {
					c.engClipPlanes = d.clipPlanes;
					d.clipPlanes = c.clipPlanes;
					if (const auto cam = d.camera.get()) {
						c.engWorld = cam->world;
						std::memcpy(c.engWorldToCam, cam->GetRuntimeData().worldToCam, sizeof(c.engWorldToCam));
						c.engData2 = cam->GetRuntimeData2();
						c.camTouched = true;
						cam->world = c.camWorld;
						std::memcpy(cam->GetRuntimeData().worldToCam, c.camWorldToCam, sizeof(c.camWorldToCam));
						cam->GetRuntimeData2() = c.camData2;
					}
				}
				if (cfg.freezeSplits) {
					d.isEnabled = c.isEnabled;
					d.port = c.port;
					auto& dir = a_light->GetShadowDirectionalLightRuntimeData();
					std::copy(g_cachedStartSplits.begin(), g_cachedStartSplits.end(), dir.startSplitDistances);
					std::copy(g_cachedEndSplits.begin(), g_cachedEndSplits.end(), dir.endSplitDistances);
				}
				if (cfg.noClear) {
					// Nur fuer diesen Frame; nach Render wird der Engine-Wert wiederhergestellt (AfterSunRender)
					c.clearSaved = d.clearRenderTarget;
					c.clearTouched = true;
					d.clearRenderTarget = false;
				}
				Stats::Count(Stats::Counter::CascadeSkipFrames);
			} else {
				// Normal-Frame: Engine-Werte unangetastet lassen, nur Stand merken
				++g_clearFlagSeen[d.clearRenderTarget ? 1 : 0];
				if (c.valid) {
					Compare(g_diffNormal, d, c, a_light);
				}
				c.lightTransform = d.lightTransform;
				c.port = d.port;
				c.isEnabled = d.isEnabled;
				if (i == cfg.cascade) {
					const auto& dir = a_light->GetShadowDirectionalLightRuntimeData();
					std::copy(std::begin(dir.startSplitDistances), std::end(dir.startSplitDistances), g_cachedStartSplits.begin());
					std::copy(std::begin(dir.endSplitDistances), std::end(dir.endSplitDistances), g_cachedEndSplits.begin());
				}
				c.clipPlanes = d.clipPlanes;
				if (const auto cam = d.camera.get()) {
					c.camWorld = cam->world;
					std::memcpy(c.camWorldToCam, cam->GetRuntimeData().worldToCam, sizeof(c.camWorldToCam));
					c.camData2 = cam->GetRuntimeData2();
				}
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
			ReportCulled();
			logger::info("[Cascade-Diag]   Cache-Invalidierungen (Ziel/Slice gewechselt): {} | Normal-Frames ferne Kaskade clearRenderTarget false/true: {}/{}",
				g_cacheInvalidations, g_clearFlagSeen[0], g_clearFlagSeen[1]);
			g_clearFlagSeen = {};
			logger::info("[Cascade-Diag]   Render veraendert lightTransform nach Accumulate: {}/{} (max {:.4f})", g_renderChangedMatrix, g_renderChecked, g_renderChangedMax);
			logger::info("[Cascade-Diag]   Projektion gleich wie im letzten gezeichneten Frame: {}/{} Frames", g_projSame, g_projChecked);
			g_projSame = g_projChecked = 0;
			g_renderChangedMatrix = g_renderChecked = 0;
			g_renderChangedMax = 0.0f;
			LogDiff("Cache-Frame vs. letzter gezeichneter", g_diffCache);
			LogDiff("Gezeichnet vs. vorheriger gezeichneter", g_diffNormal);
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
