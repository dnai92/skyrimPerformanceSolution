#include "ShadowCulling.h"
#include "Features.h"

#include "Config.h"
#include "TextureStream.h"
#include "DetourHelper.h"
#include "EngineTimers.h"
#include "FarShadowGpu.h"
#include "GpuTimer.h"
#include "LightShadowCache.h"
#include "Occlusion.h"
#include "SceneUtil.h"
#include "Stats.h"

#include <mutex>
#include <unordered_map>

namespace ShadowCulling
{
	namespace
	{
		constexpr std::size_t kMaxCascades = 4;
		constexpr std::size_t kMaxPointCameras = 48;  // Schattenkarten-Kameras aller aktiven Punkt-/Spotlichter
#ifdef SPS_VR
		// VR: jeder Schattenkarten-Deskriptor hat zwei Kameras (ShadowmapDescriptorVR::camera[2])
		constexpr std::size_t kCamsPerDesc = 2;
#else
		constexpr std::size_t kCamsPerDesc = 1;
#endif
		constexpr std::size_t kSunCameraSlots = kMaxCascades * kCamsPerDesc;

		// Vom Main-Thread pro Frame gesetzt, von den Culling-Jobs (Worker-Threads) gelesen.
		// Zuordnung ueber die Kamera: parallele Culling-Jobs nutzen eigene Culler-Instanzen,
		// aber dieselbe Kamera wie der Schattenkarten-Deskriptor.
		std::array<std::atomic<const RE::NiCamera*>, kSunCameraSlots>  g_sunCameras{};  // Kaskade i: Slots i*kCamsPerDesc ...
		std::array<std::atomic<const RE::NiCamera*>, kMaxPointCameras> g_pointCameras{};
		std::atomic<std::uint32_t>                                     g_pointCameraCount{ 0 };
		std::array<RE::NiPoint3, kMaxPointCameras>                     g_pointLightPos{};  // Licht zur Kamera (Stand Frame-Beginn)
		std::array<float, kMaxPointCameras>                            g_pointLightRadius{};

		// Stufe 0 Schatten-Cache fester Lichter (nur mit Analyse-Protokoll): Wie oft bleiben ein Licht und alles, was in
		// seine Schattenkarte kommt, von Frame zu Frame unveraendert? Die Culling-Jobs zaehlen je Schattenkamera Meshes,
		// geskinnte (Figuren, immer neu zu zeichnen) und eine reihenfolgeunabhaengige Pruefsumme der Lage aller anderen.
		struct MeshSample
		{
			const RE::BSGeometry* geom;
			std::uint64_t         hash;     // Lage (Position, Drehung, Groesse)
			std::string           name;     // Kopie (das Mesh kann bis zur Auswertung geloescht sein)
			bool                  dynamic;  // haengt an einer Figur oder ist ein Effekt -> wuerde jeden Frame neu gezeichnet
		};
		struct PointSlotDiag
		{
			std::mutex              lock;
			std::vector<MeshSample> meshes;  // ungeskinnte
			std::uint32_t           skinned = 0;
		};
		std::array<PointSlotDiag, kMaxPointCameras> g_pointDiag;
		struct HistEntry
		{
			std::uint64_t hash;
			std::string   name;
		};
		struct LightHistory
		{
			RE::NiPoint3                                         pos;
			float                                                radius = 0;
			std::unordered_map<const RE::BSGeometry*, HistEntry> meshes;  // nur statische
			std::uint32_t                                        lastFrame = 0;
		};
		std::unordered_map<const RE::NiCamera*, LightHistory> g_lightHist;  // Main-Thread
		struct LightCacheWindow
		{
			std::uint64_t maps = 0, stable = 0, moved = 0, changed = 0, setChanged = 0, fresh = 0;
			std::uint64_t meshes = 0, skinned = 0, dynamic = 0, savable = 0;
			std::unordered_map<std::string, std::uint32_t> changedNames, addedNames, removedNames, dynamicNames;
		} g_lcw;
		std::uint32_t g_lcFrame = 0;

		std::uint64_t Mix64(std::uint64_t a_x) noexcept
		{
			a_x += 0x9E3779B97F4A7C15ull;
			a_x = (a_x ^ (a_x >> 30)) * 0xBF58476D1CE4E5B9ull;
			a_x = (a_x ^ (a_x >> 27)) * 0x94D049BB133111EBull;
			return a_x ^ (a_x >> 31);
		}

		std::uint64_t Quant(float a_v, float a_scale) noexcept
		{
			return static_cast<std::uint64_t>(static_cast<std::int64_t>(a_v * a_scale));
		}

		// Haengt das Mesh an einer Figur (Waffe, Schild, Pfeile, getragener Gegenstand) oder ist es ein Effekt?
		bool IsDynamicCaster(RE::BSGeometry& a_geom) noexcept
		{
			const auto prop = a_geom.GetGeometryRuntimeData().shaderProperty.get();
			if (prop && netimmerse_cast<RE::BSEffectShaderProperty*>(prop)) {
				return true;
			}
			RE::NiAVObject* obj = &a_geom;
			for (int depth = 0; obj && depth < 32; ++depth, obj = obj->parent) {
				if (const auto ref = SceneUtil::OwnUserData(obj)) {
					return ref->IsActor();
				}
			}
			return false;
		}

		void RecordPointMesh(const RE::NiCamera* a_camera, RE::BSGeometry& a_geom) noexcept
		{
			if (!Config::analysis.load(std::memory_order_relaxed)) {
				return;
			}
			const auto count = g_pointCameraCount.load(std::memory_order_relaxed);
			for (std::uint32_t i = 0; i < count && i < kMaxPointCameras; ++i) {
				if (g_pointCameras[i].load(std::memory_order_relaxed) != a_camera) {
					continue;
				}
				auto& d = g_pointDiag[i];
				if (a_geom.GetGeometryRuntimeData().skinInstance) {
					std::scoped_lock lock(d.lock);
					++d.skinned;
					return;
				}
				const bool    dynamic = IsDynamicCaster(a_geom);
				const auto&   w = a_geom.world;
				std::uint64_t h = Mix64(Quant(w.translate.x, 4.0f));
				h = Mix64(h ^ Quant(w.translate.y, 4.0f));
				h = Mix64(h ^ Quant(w.translate.z, 4.0f));
				for (int r = 0; r < 3; ++r) {
					for (int c = 0; c < 3; ++c) {
						h = Mix64(h ^ Quant(w.rotate.entry[r][c], 1000.0f));
					}
				}
				h = Mix64(h ^ Quant(w.scale, 1000.0f));
				std::scoped_lock lock(d.lock);
				try {
					d.meshes.push_back({ &a_geom, h, a_geom.name.c_str() ? a_geom.name.c_str() : "", dynamic });
				} catch (...) {
				}
				return;
			}
		}

		void CountName(std::unordered_map<std::string, std::uint32_t>& a_map, const std::string& a_name)
		{
			if (a_map.size() < 4096 || a_map.contains(a_name)) {
				++a_map[a_name];
			}
		}

		// Frame-Beginn (Main-Thread): statische Meshes des letzten Frames je Schattenkamera mit dem Frame davor vergleichen
		void FinishLightCacheDiag() noexcept
		{
			++g_lcFrame;
			const auto  count = g_pointCameraCount.load(std::memory_order_relaxed);
			const bool  on = Config::analysis.load(std::memory_order_relaxed);
			static bool wasOn = false;
			if (!on && !wasOn) {
				return;  // nichts gesammelt (RecordPointMesh nur mit Analyse-Protokoll)
			}
			wasOn = on;  // nach dem Ausschalten einmal leeren
			for (std::uint32_t i = 0; i < count && i < kMaxPointCameras; ++i) {
				auto&                   d = g_pointDiag[i];
				std::vector<MeshSample> meshes;
				std::uint32_t           skinned = 0;
				{
					std::scoped_lock lock(d.lock);
					meshes.swap(d.meshes);
					skinned = std::exchange(d.skinned, 0u);
				}
				const auto cam = g_pointCameras[i].load(std::memory_order_relaxed);
				if (!on || !cam || (meshes.empty() && skinned == 0)) {
					continue;
				}
				try {
					auto&       h = g_lightHist[cam];
					const auto& p = g_pointLightPos[i];
					const bool  known = h.lastFrame + 1 == g_lcFrame;
					const float dx = p.x - h.pos.x, dy = p.y - h.pos.y, dz = p.z - h.pos.z;
					const bool  moved = known && (dx * dx + dy * dy + dz * dz > 1.0f || std::abs(g_pointLightRadius[i] - h.radius) > 1.0f);
					const bool  names = known && !moved;
					std::uint32_t changed = 0, added = 0, matched = 0, dynamic = 0, statics = 0;
					std::unordered_map<const RE::BSGeometry*, HistEntry> now;
					now.reserve(meshes.size());
					for (auto& m : meshes) {
						if (m.dynamic) {
							++dynamic;
							if (names) {
								CountName(g_lcw.dynamicNames, m.name);
							}
							continue;
						}
						++statics;
						if (const auto it = h.meshes.find(m.geom); it != h.meshes.end()) {
							++matched;
							if (it->second.hash != m.hash) {
								++changed;
								if (names) {
									CountName(g_lcw.changedNames, m.name);
								}
							}
							it->second.hash = 0;  // als gesehen markieren (Name bleibt fuer die Entfernt-Suche gueltig)
						} else {
							++added;
							if (names) {
								CountName(g_lcw.addedNames, m.name);
							}
						}
						now[m.geom] = { m.hash, std::move(m.name) };
					}
					std::uint32_t removed = 0;
					if (matched < h.meshes.size()) {
						for (const auto& [geom, e] : h.meshes) {
							if (!now.contains(geom)) {
								++removed;
								if (names) {
									CountName(g_lcw.removedNames, e.name);
								}
							}
						}
					}
					++g_lcw.maps;
					g_lcw.meshes += meshes.size() + skinned;
					g_lcw.skinned += skinned;
					g_lcw.dynamic += dynamic;
					if (!known) {
						++g_lcw.fresh;
					} else if (moved) {
						++g_lcw.moved;
					} else if (added || removed) {
						++g_lcw.setChanged;
					} else if (changed) {
						++g_lcw.changed;
					} else {
						++g_lcw.stable;
						g_lcw.savable += statics;  // Figuren und bewegliche Meshes werden weiter jeden Frame gezeichnet
					}
					h.pos = p;
					h.radius = g_pointLightRadius[i];
					h.meshes.swap(now);
					h.lastFrame = g_lcFrame;
				} catch (...) {
				}
			}
			if (g_lcFrame % 600 == 0) {
				std::erase_if(g_lightHist, [](const auto& a_e) { return a_e.second.lastFrame + 600 < g_lcFrame; });
			}
		}

		void LogTop(const char* a_what, const std::unordered_map<std::string, std::uint32_t>& a_map, std::size_t a_n)
		{
			std::vector<std::pair<std::string, std::uint32_t>> v(a_map.begin(), a_map.end());
			std::ranges::sort(v, [](const auto& a, const auto& b) { return a.second > b.second; });
			for (std::size_t k = 0; k < v.size() && k < a_n; ++k) {
				logger::info("[LightCache-Diag]     {:8} {:6}x  {}", a_what, v[k].second, v[k].first);
			}
		}

		void ReportLightCacheDiag() noexcept
		{
			auto& w = g_lcw;
			if (w.maps == 0) {
				return;
			}
			const double m = static_cast<double>(w.maps);
			logger::info("[LightCache-Diag] point light shadow maps {} | cacheable {:.0f}% | light moved {:.0f}% | static meshes moved {:.0f}% | static meshes added/removed {:.0f}% | new {:.0f}%",
				w.maps, 100.0 * w.stable / m, 100.0 * w.moved / m, 100.0 * w.changed / m, 100.0 * w.setChanged / m, 100.0 * w.fresh / m);
			logger::info("[LightCache-Diag]   meshes/map {:.0f} (characters {:.0f}, attached to characters / effects {:.0f}) | savable {:.0f}% of all point light meshes",
				w.meshes / m, w.skinned / m, w.dynamic / m, w.meshes ? 100.0 * w.savable / w.meshes : 0.0);
			LogTop("moved", w.changedNames, 8);
			LogTop("added", w.addedNames, 8);
			LogTop("removed", w.removedNames, 8);
			LogTop("dynamic", w.dynamicNames, 6);
			w = {};
		}
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

		// Sichtfeld-Culling der Sonnenschatten (Test, bViewCulling): Kann der Schatten eines Objekts das Sichtfeld der
		// Hauptkamera ueberhaupt erreichen? Die Huellkugel wird gedanklich entlang der Lichtrichtung unendlich
		// verlaengert. Liegt sie komplett ausserhalb einer Seitenebene des Sichtfelds und laeuft das Licht von dieser
		// Ebene weg, faellt der Schatten nie ins Bild - auch nicht in Lichtstrahlen/Nebel entlang der Sichtstrahlen.
		// Ebenen und Lichtrichtung setzt der Main-Thread vor dem Sonnen-Accumulate, die Culling-Jobs lesen danach.
		struct ViewPlanes
		{
			float n[4][3]{};  // nach innen zeigend, normiert
			float d[4]{};
			bool  usable[4]{};  // Licht laeuft von der Ebene weg (n . L <= 0) -> darf cullen
		};
		ViewPlanes        g_view;
		std::atomic<bool> g_viewValid{ false };
		// Rand: veraltete Huellen geskinnter Figuren (sitzende NPCs), weiche Schattenraender, Bewegung im selben Frame
		constexpr float kViewMargin = 256.0f;

		float PlaneDist(int a_i, const RE::NiPoint3& a_p) noexcept
		{
			return g_view.n[a_i][0] * a_p.x + g_view.n[a_i][1] * a_p.y + g_view.n[a_i][2] * a_p.z + g_view.d[a_i];
		}

		bool ShadowOutsideView(const RE::NiBound& a_bound) noexcept
		{
			if (!g_viewValid.load(std::memory_order_acquire) || a_bound.radius <= 0.0f) {
				return false;
			}
			const float r = a_bound.radius + kViewMargin;
			for (int i = 0; i < 4; ++i) {
				if (g_view.usable[i] && PlaneDist(i, a_bound.center) < -r) {
					return true;
				}
			}
			return false;
		}

		// Kontrolle: Objekt der Hauptszene komplett ausserhalb der Ebenen? (sollte bei richtigen Ebenen nie vorkommen)
		bool OutsideViewPlanes(const RE::NiBound& a_bound) noexcept
		{
			if (!g_viewValid.load(std::memory_order_acquire) || a_bound.radius <= 0.0f) {
				return false;
			}
			for (int i = 0; i < 4; ++i) {
				if (PlaneDist(i, a_bound.center) < -a_bound.radius - 16.0f) {
					return true;
				}
			}
			return false;
		}

		void UpdateViewPlanes(RE::BSShadowDirectionalLight* a_light) noexcept
		{
			g_viewValid.store(false, std::memory_order_relaxed);
			if (!Config::sunViewCulling.load(std::memory_order_relaxed) || !Config::masterEnabled.load(std::memory_order_relaxed) || !a_light) {
				return;
			}
			const auto  cam = RE::Main::WorldRootCamera();
			const auto& descs = a_light->GetRuntimeData().shadowmapDescriptors;
			if (!cam || descs.empty() || !descs[0].camera) {
				return;
			}
			const auto& fr = cam->GetRuntimeData2().viewFrustum;
			// nur symmetrische Perspektive: dann ist egal, ob Spalte 1/2 der Kamera nach oben/rechts oder gespiegelt zeigen
			const float w = fr.fRight - fr.fLeft, h = fr.fTop - fr.fBottom;
			if (fr.bOrtho || !(w > 0.0f) || !(h > 0.0f) || std::abs(fr.fRight + fr.fLeft) > 0.02f * w || std::abs(fr.fTop + fr.fBottom) > 0.02f * h) {
				return;
			}
			// Lichtrichtung = Blickrichtung der Schattenkamera (Spalte 0), muss nach unten zeigen
			const auto& lr = descs[0].camera->world.rotate;
			const float L[3]{ lr.entry[0][0], lr.entry[1][0], lr.entry[2][0] };
			const float lLen = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
			if (!(lLen > 0.5f) || L[2] / lLen > -0.02f) {
				return;
			}
			const auto& r = cam->world.rotate;
			const auto& p = cam->world.translate;
			const float D[3]{ r.entry[0][0], r.entry[1][0], r.entry[2][0] };  // Blickrichtung
			const float U[3]{ r.entry[0][1], r.entry[1][1], r.entry[2][1] };
			const float R[3]{ r.entry[0][2], r.entry[1][2], r.entry[2][2] };
			const float a = fr.fRight, t = fr.fTop;
			const float planes[4][3]{
				{ a * D[0] - R[0], a * D[1] - R[1], a * D[2] - R[2] },
				{ a * D[0] + R[0], a * D[1] + R[1], a * D[2] + R[2] },
				{ t * D[0] - U[0], t * D[1] - U[1], t * D[2] - U[2] },
				{ t * D[0] + U[0], t * D[1] + U[1], t * D[2] + U[2] },
			};
			for (int i = 0; i < 4; ++i) {
				const float len = std::sqrt(planes[i][0] * planes[i][0] + planes[i][1] * planes[i][1] + planes[i][2] * planes[i][2]);
				if (!(len > 1e-4f)) {
					return;
				}
				for (int k = 0; k < 3; ++k) {
					g_view.n[i][k] = planes[i][k] / len;
				}
				g_view.d[i] = -(g_view.n[i][0] * p.x + g_view.n[i][1] * p.y + g_view.n[i][2] * p.z);
				g_view.usable[i] = g_view.n[i][0] * L[0] + g_view.n[i][1] * L[1] + g_view.n[i][2] * L[2] <= 0.0f;
			}
			g_viewValid.store(true, std::memory_order_release);
		}

		// Cache der fernen Kaskade (FarShadowCache). Modus je Frame (Main-Thread setzt in UpdateCamera):
		// kWait = normal zeichnen, danach vollstaendige Ebene sichern | kBuild = nur Unbewegliche zeichnen, als Cache sichern,
		// vollstaendige Ebene des Vorframes zuruecklegen | kCached = Cache einsetzen, nur Bewegliche darueber zeichnen
		enum class FarMode : int
		{
			kNormal,
			kWait,
			kBuild,
			kCached
		};
		constexpr std::uint32_t       kFarCascade = 1;
		std::atomic<int>              g_farMode{ 0 };
		int                           g_farDrawMode = 0;  // FarMode waehrend die ferne Kaskade gezeichnet wird (sonst 0)
		std::atomic<std::uint64_t>    g_farSkippedStatic{ 0 };
		bool                          FarDynamic(const RE::BSGeometry& a_geom) noexcept;  // Figuren, Angehaengtes, Effekte (weiter unten)
		RE::BSShadowDirectionalLight* g_cacheLight = nullptr;                              // Licht des laufenden Frames (Main-Thread)

		struct Projection
		{
			float         worldToCam[4][4]{};
			RE::NiFrustum frustum{};
		};

		float MaxAbsDiff(const float* a_a, const float* a_b, std::size_t a_n) noexcept
		{
			float m = 0.0f;
			for (std::size_t i = 0; i < a_n; ++i) {
				m = std::max(m, std::abs(a_a[i] - a_b[i]));
			}
			return m;
		}

		void DiagRecord(const RE::NiCamera* a_camera) noexcept
		{
			if (!Config::analysis.load(std::memory_order_relaxed)) {
				return;
			}
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
			for (std::uint32_t i = 0; i < kSunCameraSlots; ++i) {
				if (g_sunCameras[i].load(std::memory_order_relaxed) == camera) {
					a_cascade = i / static_cast<std::uint32_t>(kCamsPerDesc);
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

		// Regel nur auf die Huelle (Kugel) angewendet. Fuer einen Knoten gilt: jedes Kind liegt innerhalb der Huelle,
		// ist also hoechstens so gross und mindestens so weit entfernt -> trifft die Regel auf den Knoten zu, dann auf
		// jedes Kind (Grundlage fuer das Ueberspringen ganzer Teilbaeume).
		bool ShouldCullBound(const Config::CullRule& a_rule, const RE::NiBound& a_bound, std::uint32_t a_cascade, float a_radiusScale) noexcept
		{
			if (!a_rule.enabled || a_cascade < a_rule.minCascade || !Config::masterEnabled.load(std::memory_order_relaxed)) {
				return false;
			}
			const float radius = a_bound.radius * a_radiusScale;
			if (a_bound.radius <= 0.0f || radius >= a_rule.maxRadius) {
				return false;
			}
			const float dx = a_bound.center.x - g_camX.load(std::memory_order_relaxed);
			const float dy = a_bound.center.y - g_camY.load(std::memory_order_relaxed);
			const float dz = a_bound.center.z - g_camZ.load(std::memory_order_relaxed);
			const float distance = std::sqrt(dx * dx + dy * dy + dz * dz) - a_bound.radius;
			if (distance <= a_rule.minDistance) {
				return false;
			}
			return radius / distance < a_rule.minAngularSize;
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

		// Punktlicht: Objekte nahe am Licht werfen grosse, gut sichtbare Schatten, auch wenn sie selbst klein sind
		// (Wegweiser direkt neben dem Feuer im Pavillon vor Weisslauf). Die Schattenkamera steht im Licht.
		constexpr float kNearLightAngular = 0.1f;
		// Punktlicht-Culling an? Vor NearPointLight/PointShadowScale pruefen (beide rechnen je Mesh)
		bool PointCullOn() noexcept
		{
			return Config::pointLightCulling.enabled && Config::masterEnabled.load(std::memory_order_relaxed);
		}
		std::atomic<bool> g_pointCullAllowed{ true };  // false = Innenraum und dort nicht erlaubt  // Radius/Abstand zum Licht darueber -> Schatten immer behalten

		// Punktlicht: Der Schatten auf einer Flaeche hinter dem Objekt ist um (Abstand Licht-Flaeche / Abstand Licht-Objekt)
		// groesser als das Objekt; die Flaeche liegt hoechstens im Leuchtradius. Bewertet wird deshalb die moegliche
		// Schattengroesse (wie bei der Sonne die Schattenlaenge), nicht das Objekt - grosse Schatten an der Wand fehlten.
		float PointShadowScale(const RE::NiCamera* a_lightCamera, const RE::BSGeometry& a_geom) noexcept
		{
			const auto count = g_pointCameraCount.load(std::memory_order_relaxed);
			for (std::uint32_t i = 0; i < count && i < kMaxPointCameras; ++i) {
				if (g_pointCameras[i].load(std::memory_order_relaxed) != a_lightCamera) {
					continue;
				}
				const auto& p = g_pointLightPos[i];
				const auto& b = a_geom.worldBound;
				const float dx = b.center.x - p.x, dy = b.center.y - p.y, dz = b.center.z - p.z;
				const float d = std::max(std::sqrt(dx * dx + dy * dy + dz * dz) - b.radius, 1.0f);
				return std::clamp(g_pointLightRadius[i] / d, 1.0f, 50.0f);
			}
			return 1.0f;
		}

		bool NearPointLight(const RE::NiCamera* a_lightCamera, const RE::BSGeometry& a_geom) noexcept
		{
			if (!a_lightCamera) {
				return false;
			}
			const auto& p = a_lightCamera->world.translate;
			const auto& b = a_geom.worldBound;
			const float dx = b.center.x - p.x, dy = b.center.y - p.y, dz = b.center.z - p.z;
			const float d = std::sqrt(dx * dx + dy * dy + dz * dz) - b.radius;
			return d <= 1.0f || b.radius / d >= kNearLightAngular;
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
			if (!Config::analysis.load(std::memory_order_relaxed)) {
				return;
			}
			const float radius = a_geom.worldBound.radius;
			if (radius < kCulledDiagMinRadius) {
				return;
			}
			try {
				const char* name = a_geom.name.c_str();
				std::scoped_lock lock{ g_culledLock };
				auto& info = g_culledBig[name && *name ? name : "(no name)"];
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
			logger::info("[Culled-Diag] Largest culled sun shadow objects (radius >= {:.0f}), over 600 frames, {} distinct names:", kCulledDiagMinRadius, list.size());
			for (std::size_t i = 0; i < list.size() && i < 25; ++i) {
				const auto& [name, c] = list[i];
				logger::info("[Culled-Diag]   {:<40} radius {:6.0f} | distance {:6.0f}-{:6.0f} | radius/distance {:.3f} | cascade {} | {}x", name, c.maxRadius, c.minDist, c.maxDist, c.angular, c.cascade, c.count);
			}
		}

		// Diagnose: Texturen der Decals in der Hauptszene (zeigt, von welcher Mod sie stammen)
		struct DecalTexInfo
		{
			std::uint32_t count = 0;
			float         minDist = 1e30f;
			float         maxDist = 0.0f;
			float         maxRadius = 0.0f;
			std::string   meshName;
		};
		std::mutex                                    g_decalTexLock;
		std::unordered_map<std::string, DecalTexInfo> g_decalTex;

		void RecordDecalTexture(RE::BSGeometry& a_geom, float a_distance) noexcept
		{
			try {
				std::string tex = "(no texture)";
				if (const auto prop = a_geom.GetGeometryRuntimeData().shaderProperty.get()) {
					if (const auto lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(prop)) {
						if (const auto mat = static_cast<RE::BSLightingShaderMaterialBase*>(lighting->material)) {
							if (const auto t = mat->diffuseTexture.get(); t && t->name.c_str()) {
								tex = t->name.c_str();
							}
						}
					}
				}
				std::scoped_lock lock{ g_decalTexLock };
				auto&            info = g_decalTex[tex];
				++info.count;
				info.minDist = std::min(info.minDist, a_distance);
				info.maxDist = std::max(info.maxDist, a_distance);
				info.maxRadius = std::max(info.maxRadius, a_geom.worldBound.radius);
				if (info.meshName.empty() && a_geom.name.c_str()) {
					info.meshName = a_geom.name.c_str();
				}
			} catch (...) {
			}
		}

		void ReportDecalTextures()
		{
			std::vector<std::pair<std::string, DecalTexInfo>> list;
			{
				std::scoped_lock lock{ g_decalTexLock };
				list.assign(g_decalTex.begin(), g_decalTex.end());
				g_decalTex.clear();
			}
			std::ranges::sort(list, [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
			logger::info("[Decal-Textures] {} distinct textures (decals/frame, averaged over 600 frames):", list.size());
			for (std::size_t i = 0; i < list.size() && i < 18; ++i) {
				const auto& [tex, d] = list[i];
				logger::info("[Decal-Textures]   {:6.1f}/frame  distance {:5.0f}-{:5.0f}  radius<={:4.0f}  '{}'  {}", d.count / 600.0, d.minDist, d.maxDist, d.maxRadius, d.meshName, tex);
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

		// Skinning allein reicht nicht: Baeume/Straeucher sind ebenfalls geskinnt (Wind) -> verloren ihre Schatten (0.18.1).
		// Erster Vorfahr mit Referenz entscheidet: Figur ja, Baum/Objekt nein.
		bool BelongsToActor(const RE::BSGeometry& a_geom) noexcept
		{
			const RE::NiAVObject* obj = &a_geom;
			for (int depth = 0; obj && depth < 32; ++depth, obj = obj->parent) {
				if (const auto ref = SceneUtil::OwnUserData(obj)) {
					return ref->GetFormType() == RE::FormType::ActorCharacter;
				}
			}
			return false;
		}

		// Figur zu einem Mesh (erster Vorfahr mit Referenz), sonst nullptr
		const RE::TESObjectREFR* OwningActor(const RE::BSGeometry& a_geom) noexcept
		{
			const RE::NiAVObject* obj = &a_geom;
			for (int depth = 0; obj && depth < 32; ++depth, obj = obj->parent) {
				if (const auto ref = SceneUtil::OwnUserData(obj)) {
					return ref->GetFormType() == RE::FormType::ActorCharacter ? ref : nullptr;
				}
			}
			return nullptr;
		}

		// Letzte Entscheidung je Figur (Hysterese). Kleine Tabelle mit atomaren Eintraegen (Zeiger | weggelassen-Bit), da
		// die Culling-Jobs parallel laufen; Kollisionen kosten nur die Hysterese, nie die Korrektheit.
		std::array<std::atomic<std::uintptr_t>, 512> g_actorShadowState{};

		// Bis 1.0.59 je Mesh mit dessen eigener Entfernung entschieden: an der Grenze fielen einzelne Koerperteile weg,
		// durch Animation pendelten sie hin und her (fernes NPC-Schatten-Flackern). Jetzt je Figur (Position der Figur)
		// und mit Hysterese: weg ab minDistance, zurueck erst unter 90 % davon.
		bool CullActorShadow(const RE::BSGeometry& a_geom, bool a_pointLight) noexcept
		{
			const auto& cfg = Config::actorShadowCulling;
			if (!cfg.enabled || (a_pointLight && !cfg.pointLights) || !Config::masterEnabled.load(std::memory_order_relaxed) ||
				!const_cast<RE::BSGeometry&>(a_geom).GetGeometryRuntimeData().skinInstance) {
				return false;
			}
			// Schnell raus: Mesh deutlich naeher als die Rueckkehr-Grenze -> sicher behalten (keine Suche nach der Figur)
			const float meshDist = DistanceToCamera(a_geom.worldBound);
			const float backDist = 0.9f * cfg.minDistance;
			if (meshDist + 2.0f * a_geom.worldBound.radius < backDist - 200.0f) {
				Stats::Count(Stats::Counter::ActorShadowKept);
				return false;
			}
			const auto actor = OwningActor(a_geom);
			if (!actor) {
				Stats::Count(Stats::Counter::ActorShadowKept);
				return false;
			}
			const auto  pos = actor->GetPosition();
			const float dx = pos.x - g_camX.load(std::memory_order_relaxed);
			const float dy = pos.y - g_camY.load(std::memory_order_relaxed);
			const float dz = pos.z - g_camZ.load(std::memory_order_relaxed);
			const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
			const auto  key = reinterpret_cast<std::uintptr_t>(actor) & ~std::uintptr_t{ 1 };
			auto&       slot = g_actorShadowState[(key >> 4) % g_actorShadowState.size()];
			const auto  prev = slot.load(std::memory_order_relaxed);
			const bool  wasCulled = (prev & ~std::uintptr_t{ 1 }) == key && (prev & 1) != 0;
			const bool  cull = dist > (wasCulled ? backDist : cfg.minDistance);
			slot.store(key | (cull ? 1 : 0), std::memory_order_relaxed);
			Stats::Count(cull ? Stats::Counter::ActorShadowCulled : Stats::Counter::ActorShadowKept);
			return cull;
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
			if (OutsideViewPlanes(a_geom.worldBound)) {
				Stats::Count(Stats::Counter::ViewCheckOutside);
			}
			const auto  property = a_geom.GetGeometryRuntimeData().shaderProperty.get();
			const bool  isDecal = property && property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kDecal, RE::BSShaderProperty::EShaderPropertyFlag::kDynamicDecal);
			const auto& dec = Config::decalCulling;
			if (isDecal) {
				if (Config::analysis.load(std::memory_order_relaxed)) {
					g_decalHist[Bucket(kDecalDistEdges, DistanceToCamera(a_geom.worldBound))][Bucket(kDecalRadEdges, a_geom.worldBound.radius)].fetch_add(1, std::memory_order_relaxed);
					const std::string_view name{ a_geom.name.c_str() ? a_geom.name.c_str() : "" };
					g_decalNames[name == "Decal" ? 0 : name == "DecalDirt" ? 1 : name.empty() ? 3 : 2].fetch_add(1, std::memory_order_relaxed);
					RecordDecalTexture(a_geom, DistanceToCamera(a_geom.worldBound));
				}
				// Figuren ausnehmen: Haaransaetze (Hairline) sind als Decal markiert und geskinnt - Fussabdruecke nie
				if (dec.enabled && Config::masterEnabled.load(std::memory_order_relaxed) && a_geom.worldBound.radius < dec.maxRadius &&
					!const_cast<RE::BSGeometry&>(a_geom).GetGeometryRuntimeData().skinInstance && DistanceToCamera(a_geom.worldBound) > dec.maxDistance) {
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
					if (!a_visible.GetGeometryRuntimeData().skinInstance && ShadowOutsideView(a_visible.worldBound)) {
						Stats::Count(Stats::Counter::SunViewCulled);
						return;
					}
					if (g_sunCullAllowed.load(std::memory_order_relaxed) && CullActorShadow(a_visible, false)) {
						return;
					}
					if (g_sunCullAllowed.load(std::memory_order_relaxed) && ShouldCull(Config::shadowCulling, a_visible, cascade, 1.0f / g_sunSin.load(std::memory_order_relaxed))) {
						Stats::Count(Stats::Counter::SunCulled);
						if (Config::analysis.load(std::memory_order_relaxed)) {
							RecordCulled(a_visible, cascade, DistanceToCamera(a_visible.worldBound));
						}
						return;
					}
					Stats::Count(cascade == 0 ? Stats::Counter::SunCascade0 : cascade == 1 ? Stats::Counter::SunCascade1 : Stats::Counter::SunCascade2Plus);
					break;
				case Kind::kPoint:
					if (CullActorShadow(a_visible, true)) {
						return;
					}
					// minCascade gilt nur fuer die Sonne -> hier Kaskade als "hinreichend gross" uebergeben
					if (g_pointCullAllowed.load(std::memory_order_relaxed) && PointCullOn() && !NearPointLight(a_this->camera, a_visible) &&
						ShouldCull(Config::pointLightCulling, a_visible, UINT32_MAX, PointShadowScale(a_this->camera, a_visible))) {
						Stats::Count(Stats::Counter::PointCulled);
						return;
					}
					Stats::Count(Stats::Counter::PointKept);
					RecordPointMesh(a_this->camera, a_visible);
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
				Occlusion::OnDepthPrepassBegin();
				GpuTimer::Begin(GpuTimer::kDepthPrepass);
				func(a_arg1, a_arg2);
				GpuTimer::End(GpuTimer::kDepthPrepass);
				g_inDepthPrepass.store(false, std::memory_order_relaxed);
				Occlusion::OnDepthPrepassEnd();
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
				// Figurenschatten ferner Figuren auch bei Rundum-Lichtern (Fackeln, Feuer) - bis 1.0.57 nur bei Spot-Lichtern
				if (CullActorShadow(a_visible, true)) {
					return;
				}
				if (g_pointCullAllowed.load(std::memory_order_relaxed) && PointCullOn() && !NearPointLight(a_this->camera, a_visible) &&
					ShouldCull(Config::pointLightCulling, a_visible, UINT32_MAX, PointShadowScale(a_this->camera, a_visible))) {
					Stats::Count(Stats::Counter::PointCulled);
					return;
				}
				Stats::Count(Stats::Counter::PointKept);
				RecordPointMesh(a_this->camera, a_visible);
				func(a_this, a_visible, a_alphaGroupIndex);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// BSLightingShaderProperty::GetRenderPasses (vfunc 0x2A): Draws fuer die normale Darstellung (Hauptszene,
		// Reflexions-Cubemaps). Community Shaders (TruePBR) haengt sich ebenfalls hier ein und ruft das Original auf;
		// wir liegen dahinter und leeren die Liste fuer ferne kleine Decals bzw. winzige Objekte.
		bool BelongsToActorNode(const RE::NiAVObject* a_obj) noexcept;

		// Spiel pausiert (Inventar, Container, Handel ...): das 3D-Modell im Menue wird mit eigener Kamera gezeichnet,
		// die Abstaende zur Weltkamera stimmen dann nicht -> im Hauptbild nichts weglassen (fehlende Inventarbilder 1.0.50).
		// Direkt abfragen: OnFrame haengt am Spieler-Update und laeuft bei pausiertem Spiel nicht (1.0.53 griff deshalb nie).
		// Hauptwelt-Accumulator: der mit den meisten Objekten in der letzten Sekunde. Mikro-Culling und Decals nur dort -
		// GetRenderPasses laeuft auch fuer Vorschau-Modelle (modex, Inventar ohne Pause), Spiegelungen und Menue-3D, deren
		// Abstand zur Weltkamera nichts bedeutet (Struempfe in der Vorschau erst nach Heranzoomen sichtbar, 1.0.50).
		struct AccCount
		{
			std::atomic<const void*>   acc{ nullptr };
			std::atomic<std::uint32_t> n{ 0 };
		};
		std::array<AccCount, 6>  g_accCounts;
		std::atomic<const void*> g_mainWorldAcc{ nullptr };

		void NoteAccumulator(const void* a_acc) noexcept
		{
			// Stichprobe (jedes 16. Objekt je Thread) - die Mehrheit genuegt, spart Zugriffe auf gemeinsame Zaehler
			thread_local std::uint32_t tick = 0;
			if (!a_acc || (++tick & 15) != 0) {
				return;
			}
			for (auto& c : g_accCounts) {
				if (c.acc.load(std::memory_order_relaxed) == a_acc) {
					c.n.fetch_add(1, std::memory_order_relaxed);
					return;
				}
			}
			for (auto& c : g_accCounts) {
				const void* expected = nullptr;
				if (c.acc.compare_exchange_strong(expected, a_acc) || expected == a_acc) {
					c.n.fetch_add(1, std::memory_order_relaxed);
					return;
				}
			}
		}

		bool IsMainWorldAccumulator(const void* a_acc) noexcept
		{
			return a_acc && a_acc == g_mainWorldAcc.load(std::memory_order_relaxed);
		}

		void UpdateMainWorldAccumulator() noexcept
		{
			static auto last = std::chrono::steady_clock::now();
			const auto  now = std::chrono::steady_clock::now();
			if (now - last < std::chrono::seconds(1)) {
				return;
			}
			last = now;
			const void*   best = nullptr;
			std::uint32_t most = 0;
			for (auto& c : g_accCounts) {
				const auto n = c.n.exchange(0, std::memory_order_relaxed);
				if (n > most) {
					most = n;
					best = c.acc.load(std::memory_order_relaxed);
				}
				if (n == 0) {
					c.acc.store(nullptr, std::memory_order_relaxed);  // Platz fuer neue Accumulatoren freigeben
				}
			}
			g_mainWorldAcc.store(best, std::memory_order_relaxed);
		}

		// ---- Diagnose: Liste der im Hauptbild weggelassenen Objekte ----
		std::atomic<int>                                          g_microListFrames{ 0 };
		std::mutex                                                g_microListLock;
		std::unordered_map<const RE::BSGeometry*, std::string>    g_microList;
		int                                                       g_microListNumber = 0;

		void RecordMicroCulled(const RE::BSGeometry& a_geom, const void* a_acc) noexcept
		{
			try {
				{
					std::scoped_lock lock(g_microListLock);
					if (g_microList.size() >= 4000 || g_microList.contains(&a_geom)) {
						return;
					}
				}
				const auto  property = a_geom.GetGeometryRuntimeData().shaderProperty.get();
				const bool  decal = property && property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kDecal, RE::BSShaderProperty::EShaderPropertyFlag::kDynamicDecal);
				std::string path;
				RE::TESObjectREFR* ref = nullptr;
				const RE::NiAVObject* obj = a_geom.parent;
				for (int depth = 0; obj && depth < 32; ++depth, obj = obj->parent) {
					if (!ref) {
						ref = SceneUtil::OwnUserData(obj);
					}
					if (depth < 6) {
						path += obj->name.c_str() ? obj->name.c_str() : "";
						path += '/';
					} else if (!obj->parent) {
						path += "../";
						path += obj->name.c_str() ? obj->name.c_str() : "";  // Wurzel: WorldRoot, Kamera, Menue-Szene ...
					}
				}
				const auto& b = a_geom.worldBound;
				const float dx = b.center.x - g_camX.load(std::memory_order_relaxed), dy = b.center.y - g_camY.load(std::memory_order_relaxed),
							dz = b.center.z - g_camZ.load(std::memory_order_relaxed);
				std::string line = std::format("{};{};{};{:08X};{};{:.0f};{:.0f};{:.0f};{:.1f};{:.0f};{}", decal ? "decal" : "micro",
					a_geom.name.c_str() ? a_geom.name.c_str() : "", ref ? ref->GetName() : "", ref ? ref->GetFormID() : 0u, path, b.center.x, b.center.y, b.center.z,
					b.radius, std::sqrt(dx * dx + dy * dy + dz * dz), a_acc);
				std::ranges::replace(line, '\n', ' ');
				std::scoped_lock lock(g_microListLock);
				g_microList.try_emplace(&a_geom, std::move(line));
			} catch (...) {
			}
		}

		void FinishMicroCullList() noexcept
		{
			try {
				std::unordered_map<const RE::BSGeometry*, std::string> list;
				{
					std::scoped_lock lock(g_microListLock);
					list.swap(g_microList);
				}
				const auto dir = SKSE::log::log_directory();
				if (!dir) {
					return;
				}
				const auto    path = *dir / std::format("SPS_MicroCulled_{}.csv", ++g_microListNumber);
				std::ofstream f(path);
				f << "rule;name;ref;formid;parents;cx;cy;cz;radius;distance;accumulator\n";
				for (const auto& [k, line] : list) {
					f << line << "\n";
				}
				logger::info("[MainViewCulling] list of left out objects saved: {} ({} objects, 30 frames)", path.string(), list.size());
			} catch (...) {
			}
		}

		bool GamePaused() noexcept
		{
			const auto ui = RE::UI::GetSingleton();
			return ui && ui->GameIsPaused();
		}

		struct LightingRenderPasses
		{
			static RE::BSShaderProperty::RenderPassArray* thunk(RE::BSLightingShaderProperty* a_this, RE::BSGeometry* a_geometry, std::uint32_t a_renderFlags, RE::BSShaderAccumulator* a_accumulator)
			{
				// Die Liste gehoert der Property und wird ueber lastRenderPassState zwischengespeichert: das Original baut sie
				// nur neu, wenn sich der Zustand (z. B. Lichter) aendert. Leeren wir sie, bleibt das Objekt in den Folgeframes
				// ohne Beleuchtungs-Draws (nur Tiefe -> graue Silhouette), bis es nah herankommt oder neu geladen wird.
				// Deshalb eine leere Liste zurueckgeben und den Zwischenspeicher unangetastet lassen.
				thread_local RE::BSShaderProperty::RenderPassArray empty{ nullptr, 0 };
				const auto passes = func(a_this, a_geometry, a_renderFlags, a_accumulator);
				if (GamePaused()) {
					return passes;
				}
				NoteAccumulator(a_accumulator);
				if (passes && passes->head && a_geometry && IsMainWorldAccumulator(a_accumulator) && ShouldCullMain(*a_geometry)) {
					if (g_microListFrames.load(std::memory_order_relaxed) > 0) {
						RecordMicroCulled(*a_geometry, a_accumulator);
					}
					empty.head = nullptr;
					return &empty;
				} else if (passes && passes->head && a_geometry) {
					// Anzahl der Draws nur fuer die Verdeckungs-Statistik (Analyse-Protokoll / Messung)
					std::uint32_t draws = 1;
					if (Config::analysis.load(std::memory_order_relaxed) || Config::occlusionProbe.load(std::memory_order_relaxed)) {
						draws = 0;
						for (auto p = passes->head; p && draws < 64; p = p->next) {
							++draws;
						}
					}
					// Teile an Figuren (Waffen, Schilde, Trommel auf dem Ruecken) bewegen sich mit der Animation -> nie weglassen
					if (Occlusion::CountMain(*a_geometry, a_accumulator, draws) && !BelongsToActorNode(a_geometry)) {
						empty.head = nullptr;
						return &empty;
					}
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
				// Wie beim Hauptbild: leere Liste zurueckgeben statt Clear(). Clear() gibt die Passes an den Pool zurueck,
				// waehrend die Engine sie evtl. noch eingetragen hat -> spaeter wird ein wiederverwendeter Pass gezeichnet (CTD).
				thread_local RE::BSShaderProperty::RenderPassArray empty{ nullptr, 0 };
				const auto passes = func(a_this, a_geometry, a_renderMode, a_accumulator);
				const auto& cfg = Config::skylightingCulling;
				if (passes && a_geometry && passes->head) {
					const bool ab = EngineTimers::SkylightABRunning();
					if (cfg.enabled && Config::masterEnabled.load(std::memory_order_relaxed) && a_geometry->worldBound.radius < cfg.minRadius) {
						Stats::Count(Stats::Counter::SkylightCulled);
						if (ab) {
							EngineTimers::CountSkylight(true);
						}
						empty.head = nullptr;
						return &empty;
					} else {
						Stats::Count(Stats::Counter::SkylightKept);
						if (ab) {
							EngineTimers::CountSkylight(false);
						}
					}
				}
				return passes;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// ---- Teilbaeume ueberspringen (Process = vfunc 0x16, wird fuer jeden Knoten/jedes Objekt im Durchlauf aufgerufen) ----
		// Figuren nie ueberspringen (ihre Schatten haben eigene Regeln). Geprueft wird nur fuer Knoten, die sonst
		// uebersprungen wuerden: erster Vorfahr mit Referenz entscheidet. Bis 1.0.41 wurde jedes Objekt jedes Durchlaufs
		// vorab geprueft (Lesezugriff ausserhalb des Caches) - ~2 % des Haupt-Threads am Markt (Tracy)
		bool BelongsToActorNode(const RE::NiAVObject* a_obj) noexcept
		{
			for (int depth = 0; a_obj && depth < 32; ++depth, a_obj = a_obj->parent) {
				if (const auto ref = SceneUtil::OwnUserData(a_obj)) {
					return ref->GetFormType() == RE::FormType::ActorCharacter;
				}
			}
			return false;
		}


		// Kamera der Niederschlags-/Skylighting-Verdeckungskarte. Erkennung ueber die Kamera, nicht ueber den Culler:
		// das Spiel verteilt den Durchlauf auf Kopien des Cullers (Zeigervergleich traf nie).
		const RE::NiCamera* PrecipCamera() noexcept
		{
			const auto sky = RE::Sky::GetSingleton();
			return sky && sky->precip ? sky->precip->occlusionData.camera.get() : nullptr;
		}

		// true = Knoten samt Inhalt ueberspringen; a_sun = Sonnen-Durchlauf (dort gelten Figuren-/Skinning-Ausnahmen)
		bool ShouldPrune(RE::BSCullingProcess* a_this, RE::NiAVObject* a_obj, bool a_parabolic, bool& a_sun) noexcept
		{
			a_sun = false;
			if (!a_obj || !Config::subtreePruning.enabled || !Config::masterEnabled.load(std::memory_order_relaxed)) {
				return false;
			}
			const auto node = a_obj->AsNode();
			if (!node) {
				return false;
			}
			const auto& bound = node->worldBound;
			if (bound.radius <= 0.0f) {
				return false;
			}
			const auto& sp = Config::subtreePruning;
			if (a_parabolic) {
				if (sp.point && ShouldCullBound(Config::pointLightCulling, bound, UINT32_MAX, 1.0f)) {
					Stats::Count(Stats::Counter::PrunedPoint);
					return true;
				}
				return false;
			}
			if (a_this->camera && a_this->camera == PrecipCamera()) {
				const auto& sky = Config::skylightingCulling;
				if (sp.precip && sky.enabled && bound.radius < sky.minRadius) {
					Stats::Count(Stats::Counter::PrunedPrecip);
					return true;
				}
				return false;
			}
			std::uint32_t cascade = 0;
			switch (Classify(a_this, cascade)) {
			case Kind::kSun:
				if (ShadowOutsideView(bound)) {
					Stats::Count(Stats::Counter::PrunedSunView);
					a_sun = true;
					return true;
				}
				if (sp.sun && g_sunCullAllowed.load(std::memory_order_relaxed) &&
					ShouldCullBound(Config::shadowCulling, bound, cascade, 1.0f / g_sunSin.load(std::memory_order_relaxed))) {
					Stats::Count(Stats::Counter::PrunedSun);
					a_sun = true;
					return true;
				}
				return false;
			case Kind::kPoint:
				if (sp.point && ShouldCullBound(Config::pointLightCulling, bound, UINT32_MAX, 1.0f)) {
					Stats::Count(Stats::Counter::PrunedPoint);
					return true;
				}
				return false;
			default:
				return false;
			}
		}

		// Enthaelt der Teilbaum Figuren oder geskinnte Meshes (Stoff-Banner, Figuren in Gruppenknoten)? Die Regel fuer
		// einzelne Meshes nimmt sie aus (bSkipSkinned, eigene Figuren-Regel) - das Ueberspringen ganzer Knoten tat das bis
		// 1.0.55 nicht. Nur fuer Knoten geprueft, die sonst uebersprungen wuerden (klein, weit weg -> wenige Kinder);
		// zu grosse Teilbaeume gelten vorsichtshalber als "enthaelt".
		bool ContainsSkinnedOrActor(RE::NiAVObject* a_root) noexcept
		{
			std::array<RE::NiAVObject*, 64> stack;
			std::size_t                     n = 0, visited = 0;
			stack[n++] = a_root;
			while (n > 0) {
				const auto obj = stack[--n];
				if (!obj) {
					continue;
				}
				if (++visited > 128) {
					return true;
				}
				if (const auto ref = SceneUtil::OwnUserData(obj); ref && ref->GetFormType() == RE::FormType::ActorCharacter) {
					return true;
				}
				if (const auto geom = obj->AsGeometry()) {
					if (geom->GetGeometryRuntimeData().skinInstance) {
						return true;
					}
				} else if (const auto node = obj->AsNode()) {
					for (const auto& child : node->GetChildren()) {
						if (child) {
							if (n >= stack.size()) {
								return true;
							}
							stack[n++] = child.get();
						}
					}
				}
			}
			return false;
		}

		// Ergebnis je Knoten zwischenspeichern (je Culling-Thread, ohne Sperre): die Pruefung durchlaeuft den Teilbaum und
		// kostete im Sonnen-Durchlauf jedes Frame fast so viel, wie das Ueberspringen spart (Tracy Markt 14.10.).
		// Knoten-Inhalt aendert sich selten -> 2 s gueltig, Kinderzahl als Plausibilitaet gegen wiederverwendete Adressen.
		std::atomic<std::uint32_t> g_pruneFrame{ 0 };
		bool ContainsSkinnedOrActorCached(RE::NiAVObject* a_obj) noexcept
		{
			struct Entry
			{
				std::uint32_t frame;
				std::uint32_t children;
				bool          result;
			};
			thread_local std::unordered_map<const RE::NiAVObject*, Entry> cache;
			thread_local std::uint32_t                                   lastClear = 0;
			const auto                                                   frame = g_pruneFrame.load(std::memory_order_relaxed);
			if (frame - lastClear > 1200) {  // gelegentlich ganz leeren (entladene Zellen)
				cache.clear();
				lastClear = frame;
			}
			const auto           node = a_obj ? a_obj->AsNode() : nullptr;
			const std::uint32_t children = node ? static_cast<std::uint32_t>(node->GetChildren().size()) : 0;
			try {
				auto& e = cache[a_obj];
				if (e.frame != 0 && frame - e.frame < 120 && e.children == children) {
					return e.result;
				}
				e = { frame == 0 ? 1u : frame, children, ContainsSkinnedOrActor(a_obj) };
				return e.result;
			} catch (...) {
				return true;
			}
		}

		// Bewegliches fuer den Cache der fernen Kaskade: geskinnt, Effekt oder Teil einer Figur. Zugehoerigkeit zu einer Figur
		// je Mesh zwischengespeichert (Elternkette bis 32 Ebenen, ~1600 Meshes je Frame in der fernen Kaskade)
		bool FarDynamic(const RE::BSGeometry& a_geom) noexcept
		{
			auto& data = const_cast<RE::BSGeometry&>(a_geom).GetGeometryRuntimeData();
			if (data.skinInstance || netimmerse_cast<RE::BSDynamicTriShape*>(const_cast<RE::BSGeometry*>(&a_geom))) {
				return true;
			}
			if (const auto prop = data.shaderProperty.get(); prop && netimmerse_cast<RE::BSEffectShaderProperty*>(prop)) {
				return true;
			}
			struct Entry
			{
				std::uint32_t frame;
				bool          actor;
			};
			thread_local std::unordered_map<const RE::BSGeometry*, Entry> cache;
			thread_local std::uint32_t                                   lastClear = 0;
			const auto                                                   frame = g_pruneFrame.load(std::memory_order_relaxed);
			if (frame - lastClear > 1200) {
				cache.clear();
				lastClear = frame;
			}
			try {
				auto& e = cache[&a_geom];
				if (e.frame != 0 && frame - e.frame < 120) {
					return e.actor;
				}
				e = { frame == 0 ? 1u : frame, BelongsToActorNode(&a_geom) };
				return e.actor;
			} catch (...) {
				return true;
			}
		}

		template <int N, bool Parabolic>
		struct Process
		{
			static void thunk(RE::BSCullingProcess* a_this, RE::NiAVObject* a_obj, std::uint32_t a_arg)
			{
				// Teilbaum-Pruefung nur im Sonnen-Durchlauf: die Einzel-Regeln fuer Regen/Himmel und Punktlicht kennen keine
				// Skinning-Ausnahme, und die Pruefung kostete im Regen-/Himmels-Durchlauf messbar Zeit (A/B 1.0.56)
				bool sun = false;
				if (ShouldPrune(a_this, a_obj, Parabolic, sun) && !BelongsToActorNode(a_obj) && !(sun && ContainsSkinnedOrActorCached(a_obj))) {
					return;
				}
				func(a_this, a_obj, a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_BSCullingProcess[0] };
		AppendVirtual::func = vtbl.write_vfunc(0x18, AppendVirtual::thunk);
		logger::info("Hook installed: BSCullingProcess::AppendVirtual (vfunc 0x18)");

		REL::Relocation<std::uintptr_t> parabolicVtbl{ RE::VTABLE_BSParabolicCullingProcess[0] };
		AppendVirtualParabolic::func = parabolicVtbl.write_vfunc(0x18, AppendVirtualParabolic::thunk);
		logger::info("Hook installed: BSParabolicCullingProcess::AppendVirtual (vfunc 0x18)");
#ifdef SPS_VR
		// VR: nur das Culling einzelner Meshes (AppendVirtual, vtable-Eintrag offline gegen VR 1.4.15 verglichen: gleich).
		// Teilbaeume (Process 0x16 weicht in VR ab), Skylighting/Decals und Tiefenvorpass bleiben aus.
		Features::Report("Shadow culling (VR)", "Schatten-Culling (VR)", true);
		return;
#endif

		// Teilbaeume ueberspringen: Process (vfunc 0x16) der drei Culler-Arten
		REL::Relocation<std::uintptr_t> geomListVtbl{ RE::VTABLE_BSGeometryListCullingProcess[0] };
		Process<0, false>::func = vtbl.write_vfunc(0x16, Process<0, false>::thunk);
		Process<1, true>::func = parabolicVtbl.write_vfunc(0x16, Process<1, true>::thunk);
		Process<2, false>::func = geomListVtbl.write_vfunc(0x16, Process<2, false>::thunk);
		logger::info("Hook installed: Process (vfunc 0x16) for BSCullingProcess, BSParabolicCullingProcess, BSGeometryListCullingProcess");
	}

	void InstallLate()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_BSLightingShaderProperty[0] };
		OcclusionRenderPasses::func = vtbl.write_vfunc(0x2D, OcclusionRenderPasses::thunk);
		LightingRenderPasses::func = vtbl.write_vfunc(0x2A, LightingRenderPasses::thunk);
		logger::info("Hook installed: BSLightingShaderProperty::GetRenderPasses (vfunc 0x2A, after Community Shaders)");
		logger::info("Hook installed: BSLightingShaderProperty::GetRenderPasses_Occlusion (vfunc 0x2D, after Community Shaders)");

		// Main::RenderDepth wird nur indirekt aufgerufen (kein direkter call/jmp im Spielcode). Community Shaders leitet den
		// Funktionsanfang per Detours um; Detours verkettet einen weiteren Hook sauber dahinter.
		RenderDepth::func = reinterpret_cast<void (*)(bool, bool)>(REL::Relocation<std::uintptr_t>{ RELOCATION_ID(100421, 107139) }.address());
		if (const auto err = DetourHelper::Attach(reinterpret_cast<void**>(&RenderDepth::func), reinterpret_cast<void*>(&RenderDepth::thunk)); err == 0) {
			logger::info("Hook installed: Main::RenderDepth (Detours)");
			Features::Report("Depth pre-pass culling", "Tiefenvorpass: kleine Objekte weglassen", true);
		} else {
			Features::Report("Depth pre-pass culling", "Tiefenvorpass: kleine Objekte weglassen", false, std::format("Detours error {}", err));
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

	namespace
	{
		// ---------------- Ruhige ferne Kaskade (Voraussetzung fuer den Cache) ----------------
		// Die Engine richtet jede Kaskade in jedem Frame neu aus (UpdateCamera, AE ID 108496): Lage folgt der Sonne,
		// Ausschnitt (Ortho-Frustum l/r/t/b) dem Blick, Tiefenbereich den Objekten - ohne Festhalten gleicht keine
		// Projektion der vorigen und der Cache greift nie. Gehalten werden ein Bezugsrahmen (Drehung + Position) und ein
		// Ausschnitt, der den Engine-Ausschnitt umschliesst; neu ausgerichtet wird erst, wenn er nicht mehr passt.
		// Kamera-Achsen: Spalte 0 = Blickrichtung, left/right laufen entlang -Spalte 2, top/bottom entlang -Spalte 1
		// (gespiegelt, per worldToCam nachgemessen; mit +Spalten fehlten Schatten, Test 1.0.10).
		struct StableState
		{
			bool          valid = false;
			RE::NiMatrix3 rot{};
			RE::NiPoint3  pos{};
			float         cx = 0.0f, cy = 0.0f, hx = 0.0f, hy = 0.0f, n = 0.0f, f = 0.0f;
		};
		StableState   g_stable{};
		std::uint32_t g_stableChanged = 0, g_stableReanchor = 0;  // fuers [FarShadowCache]-Log

		RE::NiPoint3 Col(const RE::NiMatrix3& a_m, int a_c) noexcept { return { a_m.entry[0][a_c], a_m.entry[1][a_c], a_m.entry[2][a_c] }; }
		float        Dot3(const RE::NiPoint3& a_a, const RE::NiPoint3& a_b) noexcept { return a_a.x * a_b.x + a_a.y * a_b.y + a_a.z * a_b.z; }
		float        QuantUp(float a_v, float a_step) noexcept { return std::ceil(a_v / a_step) * a_step; }

		void StabilizeFarCascade(RE::BSShadowDirectionalLight* a_light) noexcept
		{
			if (!Config::farShadowCache.enabled || !Config::masterEnabled.load(std::memory_order_relaxed) || !LightShadowCache::DrawFilterReady()) {
				g_stable.valid = false;
				return;
			}
			auto& descs = a_light->GetRuntimeData().shadowmapDescriptors;
			if (kFarCascade >= descs.size()) {
				return;
			}
			auto&      desc = descs[kFarCascade];
			const auto cam = desc.camera.get();
			if (!cam) {
				return;
			}
			auto& fr = cam->GetRuntimeData2().viewFrustum;
			if (!fr.bOrtho || fr.fRight <= fr.fLeft || fr.fTop <= fr.fBottom) {
				return;
			}
			const auto&         cfg = Config::stableCascade;
			const RE::NiMatrix3 rotE = cam->world.rotate;
			const RE::NiPoint3  posE = cam->world.translate;
			const RE::NiPoint3  dirE = Col(rotE, 0), upE = -Col(rotE, 1), rightE = -Col(rotE, 2);
			// Engine-Ausschnitt: Mitte als Weltpunkt, halbe Breite/Hoehe, Tiefenbereich
			const float         exc = 0.5f * (fr.fLeft + fr.fRight), eyc = 0.5f * (fr.fTop + fr.fBottom);
			const float         ehx = 0.5f * (fr.fRight - fr.fLeft), ehy = 0.5f * (fr.fTop - fr.fBottom);
			const RE::NiPoint3  centerW = posE + rightE * exc + upE * eyc;

			auto& s = g_stable;
			bool  changed = false;
			if (!s.valid || Dot3(dirE, Col(s.rot, 0)) < std::cos(cfg.maxAngleDeg * 0.017453292f)) {
				s = {};
				s.valid = true;
				s.rot = rotE;
				s.pos = posE;
				++g_stableReanchor;
				changed = true;
			}
			const RE::NiPoint3 dirS = Col(s.rot, 0), upS = -Col(s.rot, 1), rightS = -Col(s.rot, 2);
			const RE::NiPoint3 rel = centerW - s.pos;
			const float        x = Dot3(rel, rightS), y = Dot3(rel, upS);
			const float        depthOff = Dot3(posE - s.pos, dirS);
			const float        en = fr.fNear + depthOff, ef = fr.fFar + depthOff;

			// Groesse: erst bei zu klein oder deutlich zu gross neu (Stufen von extentStep)
			const float needX = ehx + cfg.margin, needY = ehy + cfg.margin;
			if (s.hx < needX || s.hx > needX + 2.0f * cfg.margin + cfg.extentStep) {
				s.hx = QuantUp(needX * cfg.extentFactor, cfg.extentStep);
				changed = true;
			}
			if (s.hy < needY || s.hy > needY + 2.0f * cfg.margin + cfg.extentStep) {
				s.hy = QuantUp(needY * cfg.extentFactor, cfg.extentStep);
				changed = true;
			}
			// Mitte: nur verschieben, wenn der Engine-Ausschnitt nicht mehr hineinpasst - dann auf Texel gerundet
			if (changed || std::abs(x - s.cx) + ehx > s.hx || std::abs(y - s.cy) + ehy > s.hy) {
				const float texX = 2.0f * s.hx / static_cast<float>(std::max(1, std::abs(desc.port.GetWidth())));
				const float texY = 2.0f * s.hy / static_cast<float>(std::max(1, std::abs(desc.port.GetHeight())));
				s.cx = std::round(x / texX) * texX;
				s.cy = std::round(y / texY) * texY;
				changed = true;
			}
			// Tiefe: eng an der Engine, nur depthStep Spielraum je Seite (Zittern im Stand), sonst sofort nachfuehren
			if (changed || en < s.n || ef > s.f || (s.f - s.n) > (ef - en) + 4.0f * cfg.depthStep) {
				s.n = std::max(1.0f, en - cfg.depthStep);
				s.f = ef + cfg.depthStep;
				changed = true;
			}
			if (changed) {
				++g_stableChanged;
			}
			// Anwenden: Lage + Frustum setzen, Kamera neu berechnen lassen (worldToCam) - wie die Engine selbst
			cam->local.rotate = s.rot;
			cam->local.translate = s.pos;
			cam->world.rotate = s.rot;
			cam->world.translate = s.pos;
			fr.fLeft = s.cx - s.hx;
			fr.fRight = s.cx + s.hx;
			fr.fTop = s.cy + s.hy;
			fr.fBottom = s.cy - s.hy;
			fr.fNear = s.n;
			fr.fFar = s.f;
			RE::NiUpdateData ud{};
			cam->Update(ud);
			// Schattenwerfer fuer den GANZEN gehaltenen Ausschnitt sammeln: die Engine cullt sonst mit einer engen Huelle um
			// den aktuellen Sichtbereich (customCullPlanes) - beim Drehen fehlten im Cache Schatten am Rand
			if (desc.cullingProcess) {
				desc.cullingProcess->doCustomCullPlanes = false;
			}
		}
	}

	namespace
	{
		// ---------------- Cache der fernen Kaskade ----------------
		struct FarState
		{
			bool                                  valid = false;     // Cache passt zur Projektion "build"
			bool                                  prebuilt = false;  // vollstaendige Ebene des Vorframes gesichert
			bool                                  decided = false;   // fuer lastFrame schon entschieden
			Projection                            build{}, wait{};
			RE::RENDER_TARGET_DEPTHSTENCIL        target{};
			std::uint32_t                         slice = 0;
			std::uint32_t                         lastFrame = 0;
			FarMode                               last = FarMode::kNormal;
			std::uint64_t                         frames = 0, cached = 0, builds = 0, waits = 0, normal = 0;
			std::uint64_t                         waitProjChanged = 0, waitNotSaved = 0, cacheProjChanged = 0;  // Diagnose: warum kein Aufbau
			// Zeichen-Hook der fernen Kaskade (FarBeginDraw/FarEndDraw)
			void*                                 drawCtx = nullptr;
			REX::W32::ID3D11Texture2D*            drawTex = nullptr;
			bool                                  drawDone = false;
			std::uint64_t                         hookMissed = 0, notBound = 0;
			// Aenderungserkennung: Summe ueber (Mesh, gerundete Lage) aller unbeweglichen Draws im Aufbau-Frame; weicht sie im
			// Cache-Frame ab (Objekt kam hinzu, verschwand oder wurde bewegt - Tuer, Havok-Kiste), wird neu aufgebaut
			std::uint64_t                         staticSum = 0, drawSum = 0, changed = 0;
			bool                                  gpuHeld = false;  // FarShadowGpu haelt Kopien (bei Abschalten freigeben)
			std::chrono::steady_clock::time_point lastLog = std::chrono::steady_clock::now();
		};
		FarState g_far{};

		bool SameProjection(const Projection& a_a, const Projection& a_b) noexcept
		{
			return MaxAbsDiff(&a_a.worldToCam[0][0], &a_b.worldToCam[0][0], 16) <= 1e-4f && MaxAbsDiff(&a_a.frustum.fLeft, &a_b.frustum.fLeft, 6) <= 1e-4f;
		}

		void FarDecide(RE::BSShadowDirectionalLight* a_light) noexcept
		{
			auto&       f = g_far;
			const auto  frame = g_pruneFrame.load(std::memory_order_relaxed);
			if (frame == f.lastFrame && f.decided) {
				return;  // je Frame nur einmal entscheiden (falls UpdateCamera mehrfach laeuft)
			}
			f.decided = true;
			FarMode     mode = FarMode::kNormal;
			const auto& descs = a_light->GetRuntimeData().shadowmapDescriptors;
			const bool  on = Config::farShadowCache.enabled && Config::masterEnabled.load(std::memory_order_relaxed) && g_stable.valid && kFarCascade < descs.size();
			const auto cam = on ? descs[kFarCascade].camera.get() : nullptr;
			if (on && cam) {
				const auto& d = descs[kFarCascade];
				Projection  cur{};
				std::memcpy(cur.worldToCam, cam->GetRuntimeData().worldToCam, sizeof(cur.worldToCam));
				cur.frustum = cam->GetRuntimeData2().viewFrustum;
				// Luecke (Innenraum, Menue, Ladebildschirm) oder anderes Ziel: Inhalt unbrauchbar
				if (frame - f.lastFrame > 2 || d.renderTarget != f.target || d.shadowmapIndex != f.slice) {
					f.valid = false;
					f.prebuilt = false;
				}
				f.target = d.renderTarget;
				f.slice = d.shadowmapIndex;
				if (f.valid && SameProjection(cur, f.build)) {
					mode = FarMode::kCached;
				} else {
					if (f.valid) {
						++f.cacheProjChanged;
					}
					f.valid = false;
					if (f.last == FarMode::kWait && f.prebuilt && SameProjection(cur, f.wait)) {
						mode = FarMode::kBuild;
						f.build = cur;
					} else {
						if (f.last == FarMode::kWait) {
							++(f.prebuilt ? f.waitProjChanged : f.waitNotSaved);
						}
						mode = FarMode::kWait;
						f.wait = cur;
						f.prebuilt = false;
					}
				}
			} else {
				f.valid = false;
				f.prebuilt = false;
			}
			f.lastFrame = frame;
			f.last = mode;
			g_farMode.store(static_cast<int>(mode), std::memory_order_relaxed);
		}

		// UpdateCamera (Entscheidung) laeuft VOR Accumulate. Lief sie in diesem Frame nicht, gilt nichts vom Vorframe
		// (1.0.59-Test: hier wurde der Modus jedes Frame zurueckgesetzt -> Cache nie aktiv)
		void FarBeforeAccumulate() noexcept
		{
			if (g_far.lastFrame != g_pruneFrame.load(std::memory_order_relaxed)) {
				g_farMode.store(static_cast<int>(FarMode::kNormal), std::memory_order_relaxed);
			}
		}

		void FarLog() noexcept
		{
			auto& f = g_far;
			if (std::chrono::steady_clock::now() - f.lastLog < std::chrono::seconds(20) || f.frames == 0) {
				return;
			}
			f.lastLog = std::chrono::steady_clock::now();
			if (!Config::analysis.load(std::memory_order_relaxed)) {
				g_farSkippedStatic.store(0, std::memory_order_relaxed);
				f.waitProjChanged = f.waitNotSaved = f.cacheProjChanged = 0;
				f.hookMissed = f.notBound = f.changed = 0;
				g_stableChanged = g_stableReanchor = 0;
				f.frames = f.cached = f.builds = f.waits = f.normal = 0;
				return;  // Zeile nur im Analyse-Protokoll
			}
			const double n = static_cast<double>(f.frames);
			const double c = f.cached ? static_cast<double>(f.cached) : 1.0;
			logger::info("[FarShadowCache] {} frames | from cache {:.0f} % | rebuilds {} (prepared {}) | not cached {} | per cached frame: draws skipped {:.0f} | draw hook missed {} (other target {}) | "
						 "no build: projection changed {}, not saved {} | cache dropped: projection changed {} | cascade realigned {} (sun moved on {}) | static objects changed {}",
				f.frames, 100.0 * static_cast<double>(f.cached) / n, f.builds, f.waits, f.normal, static_cast<double>(g_farSkippedStatic.exchange(0)) / c,
				f.hookMissed, f.notBound, f.waitProjChanged, f.waitNotSaved, f.cacheProjChanged, g_stableChanged, g_stableReanchor, f.changed);
			f.waitProjChanged = f.waitNotSaved = f.cacheProjChanged = 0;
			f.hookMissed = f.notBound = f.changed = 0;
			g_stableChanged = g_stableReanchor = 0;
			f.frames = f.cached = f.builds = f.waits = f.normal = 0;
		}

		// Nach Render: Ergebnis des Zeichen-Hooks verbuchen. Nur wenn die ferne Kaskade wirklich ueber den Hook lief, gilt
		// ihr Stand; sonst wurde normal (alles) gezeichnet und der Cache baut neu auf.
		void FarAfterRender() noexcept
		{
			if (!Config::farShadowCache.enabled) {
				if (g_far.gpuHeld) {
					FarShadowGpu::Reset();  // ~16-64 MB VRAM nicht dauerhaft halten
					g_far.gpuHeld = false;
					g_far.valid = g_far.prebuilt = false;
				}
				return;
			}
			auto&      f = g_far;
			const auto mode = static_cast<FarMode>(g_farMode.load(std::memory_order_relaxed));
			++f.frames;
			switch (mode) {
			case FarMode::kWait:
				f.prebuilt = f.drawDone;
				++f.waits;
				break;
			case FarMode::kBuild:
				f.valid = f.drawDone;
				++f.builds;
				break;
			case FarMode::kCached:
				if (f.drawDone) {
					++f.cached;
				} else {
					f.valid = false;
					++f.normal;
				}
				break;
			default:
				++f.normal;
				break;
			}
			if (mode != FarMode::kNormal && !f.drawDone) {
				++f.hookMissed;
			}
			f.drawDone = false;
			FarLog();
		}
	}

	// Zeichnen einer einzelnen Schattenkarte (ID 107604+0x167, Hook im Schatten-Cache fester Lichter). Fuer die ferne
	// Sonnenkaskade: eingesammelt wird alles wie ohne Cache, weggelassen wird erst beim einzelnen Draw (FarSkipDraw) -
	// das Weglassen beim Einsammeln nahm Boden und Wegen den Schatten (1.0.59-Test, Testmodus 1 vs. 2), dieselbe
	// Erfahrung wie beim Cache fester Lichter (1.0.33).
	bool FarBeginDraw(const RE::NiCamera* a_camera) noexcept
	{
		auto& f = g_far;
		g_farDrawMode = 0;
		if (!a_camera || !Config::farShadowCache.enabled || !g_cacheLight) {
			return false;
		}
		const auto mode = static_cast<FarMode>(g_farMode.load(std::memory_order_relaxed));
		if (mode == FarMode::kNormal) {
			return false;
		}
		const auto& descs = g_cacheLight->GetRuntimeData().shadowmapDescriptors;
		if (kFarCascade >= descs.size() || descs[kFarCascade].camera.get() != a_camera) {
			return false;
		}
		// Waehrend einer Pause laeuft keine neue Entscheidung (Spiel-Frame steht), die Kaskade kann sich aber bewegen
		// (Freikamera): Cache bzw. Aufbau nur, wenn die Projektion noch zu ihm passt
		if (mode != FarMode::kWait) {
			Projection cur{};
			std::memcpy(cur.worldToCam, a_camera->GetRuntimeData().worldToCam, sizeof(cur.worldToCam));
			cur.frustum = a_camera->GetRuntimeData2().viewFrustum;
			if (!SameProjection(cur, mode == FarMode::kCached ? f.build : f.wait)) {
				f.valid = f.prebuilt = false;
				return false;
			}
		}
		const auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		const auto ctx = renderer ? renderer->GetRuntimeData().context : nullptr;
		if (!ctx) {
			return false;
		}
		const auto tex = renderer->GetDepthStencilData().depthStencils[static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM)].texture;
		// Nur die Sonnenkarte selbst (nicht etwa die Volumetric-Karte mit derselben Kamera)
		if (!tex || !FarShadowGpu::BoundSlice(ctx, tex, f.slice)) {
			++f.notBound;
			return false;
		}
		bool recreated = false;
		if (!FarShadowGpu::Ensure(tex, recreated)) {
			return false;
		}
		f.gpuHeld = true;
		if (recreated && mode != FarMode::kWait) {
			f.valid = f.prebuilt = false;  // neue Kopien sind leer
			return false;
		}
		if (mode == FarMode::kCached) {
			FarShadowGpu::Restore(ctx, FarShadowGpu::kStatic, tex, f.slice);  // nach dem Leeren: Unbewegliches einsetzen
		}
		f.drawCtx = ctx;
		f.drawTex = tex;
		f.drawSum = 0;
		g_farDrawMode = static_cast<int>(mode);
		return true;
	}

	void FarEndDraw() noexcept
	{
		auto&      f = g_far;
		const auto mode = static_cast<FarMode>(g_farDrawMode);
		g_farDrawMode = 0;
		if (!f.drawCtx || !f.drawTex) {
			return;
		}
		switch (mode) {
		case FarMode::kWait:
			FarShadowGpu::Save(f.drawCtx, FarShadowGpu::kPrev, f.drawTex, f.slice);  // vollstaendige Ebene fuer den Aufbau-Frame
			break;
		case FarMode::kBuild:
			FarShadowGpu::Save(f.drawCtx, FarShadowGpu::kStatic, f.drawTex, f.slice);   // nur Unbewegliches sichern
			FarShadowGpu::Restore(f.drawCtx, FarShadowGpu::kPrev, f.drawTex, f.slice);  // sichtbar: vollstaendige Ebene des Vorframes
			f.staticSum = f.drawSum;
			break;
		case FarMode::kCached:
			if (f.drawSum != f.staticSum) {
				f.valid = false;  // Unbewegliches hat sich geaendert: naechster Frame baut neu auf
				++f.changed;
			}
			break;
		default:
			break;
		}
		f.drawDone = mode != FarMode::kNormal;
		f.drawCtx = nullptr;
		f.drawTex = nullptr;
	}

	namespace
	{
		thread_local bool t_inShadowMapDraw = false;
	}

	void SetInShadowMapDraw(bool a_in) noexcept
	{
		t_inShadowMapDraw = a_in;
	}

	// Ringe, Ohrringe, Augen, Zaehne und kleine Ruestungsteile: ihr Schatten ist ab wenigen Metern nicht zu sehen.
	// Erst beim Draw weggelassen - beim Einsammeln haette das Nebenwirkungen (Schattenempfang, siehe Cache der fernen
	// Kaskade 1.0.59), z. B. leuchtende Augen im Schatten.
	bool SkipActorPartShadowDraw(const RE::BSRenderPass& a_pass) noexcept
	{
		const auto& cfg = Config::actorPartShadowCulling;
		if (!t_inShadowMapDraw || !cfg.enabled || !a_pass.geometry || !Config::masterEnabled.load(std::memory_order_relaxed)) {
			return false;
		}
		const auto& geom = *a_pass.geometry;
		if (!const_cast<RE::BSGeometry&>(geom).GetGeometryRuntimeData().skinInstance) {
			return false;
		}
		const auto& b = geom.worldBound;
		if (b.radius <= 0.0f || b.radius >= cfg.maxRadius) {
			return false;
		}
		if (DistanceToCamera(b) > cfg.minDistance) {
			Stats::Count(Stats::Counter::ActorPartShadowCulled);
			return true;
		}
		Stats::Count(Stats::Counter::ActorPartShadowKept);
		return false;
	}

	bool FarSkipDraw(const RE::BSRenderPass& a_pass) noexcept
	{
		const auto mode = static_cast<FarMode>(g_farDrawMode);
		if ((mode != FarMode::kCached && mode != FarMode::kBuild) || !a_pass.geometry) {
			return false;
		}
		const auto& geom = *a_pass.geometry;
		const bool  dyn = FarDynamic(geom);
		if (!dyn) {
			const auto& t = geom.world.translate;
			g_far.drawSum += Mix64(reinterpret_cast<std::uintptr_t>(&geom) ^ Mix64(Quant(t.x, 0.25f) ^ (Quant(t.y, 0.25f) << 21) ^ (Quant(t.z, 0.25f) << 42)));
		}
		if (mode == FarMode::kCached && !dyn) {
			g_farSkippedStatic.fetch_add(1, std::memory_order_relaxed);
			return true;  // steckt im Cache
		}
		if (mode == FarMode::kBuild && dyn) {
			return true;  // Cache nur mit Unbeweglichem
		}
		return false;
	}

	void AfterSunUpdateCamera(RE::BSShadowDirectionalLight* a_light) noexcept
	{
		if (!a_light) {
			return;
		}
		StabilizeFarCascade(a_light);
		FarDecide(a_light);
	}

	void BeforeSunAccumulate(RE::BSShadowDirectionalLight* a_light) noexcept
	{
		UpdateViewPlanes(a_light);
		FarBeforeAccumulate();
	}

	void RequestMicroCullList() noexcept
	{
		g_microListFrames.store(30, std::memory_order_relaxed);
	}

	void AfterSunRender() noexcept
	{
		FarAfterRender();
		g_cacheLight = nullptr;  // nur bis Render-Ende gueltig; Zellwechsel/Innenraum duerfen keinen alten Zeiger sehen
	}

	void ResetWorld() noexcept
	{
		g_far = {};
		g_stable.valid = false;
		g_cacheLight = nullptr;
		g_farDrawMode = 0;
		g_farMode.store(static_cast<int>(FarMode::kNormal), std::memory_order_relaxed);
		FarShadowGpu::Reset();
		for (auto& s : g_actorShadowState) {
			s.store(0, std::memory_order_relaxed);
		}
	}

	void AfterSunAccumulate(RE::BSShadowDirectionalLight* a_light) noexcept
	{
		g_cacheLight = a_light;
	}

	void OnFrame()
	{
		Config::ReloadIfChanged();
		EngineTimers::OnFrame();
		TextureStream::OnFrame();
		g_viewValid.store(false, std::memory_order_relaxed);  // neu erst beim naechsten Sonnen-Accumulate (Innenraeume: nie)
#ifndef SPS_VR
		FinishLightCacheDiag();  // vor dem Neuaufbau der Kameraliste: Zaehler gehoeren zur Liste des letzten Frames
		LightShadowCache::OnFrame();
#endif

		UpdateMainWorldAccumulator();
		g_pruneFrame.fetch_add(1, std::memory_order_relaxed);
		if (const auto left = g_microListFrames.load(std::memory_order_relaxed); left > 0 && g_microListFrames.fetch_sub(1, std::memory_order_relaxed) == 1) {
			FinishMicroCullList();
		}
		if (const auto camera = RE::PlayerCamera::GetSingleton(); camera && camera->cameraRoot) {
			const auto& pos = camera->cameraRoot->world.translate;
			g_camX.store(pos.x, std::memory_order_relaxed);
			g_camY.store(pos.y, std::memory_order_relaxed);
			g_camZ.store(pos.z, std::memory_order_relaxed);
		}

		g_mainCamera.store(RE::Main::WorldRootCamera(), std::memory_order_relaxed);
		{
			const auto player = RE::PlayerCharacter::GetSingleton();
			const auto cell = player ? player->GetParentCell() : nullptr;
			g_pointCullAllowed.store(!cell || !cell->IsInteriorCell() || Config::pointLightInteriors.load(std::memory_order_relaxed), std::memory_order_relaxed);
		}

		std::array<const RE::NiCamera*, kSunCameraSlots> cameras{};
		std::uint32_t                                    pointCount = 0;
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
#ifdef SPS_VR
				const auto& descriptors = sun->GetVRRuntimeData().shadowmapDescriptors;
				g_descCount = descriptors.size();
				for (std::uint32_t i = 0; i < descriptors.size() && i < kMaxCascades; ++i) {
					for (std::uint32_t e = 0; e < kCamsPerDesc; ++e) {
						cameras[i * kCamsPerDesc + e] = descriptors[i].camera[e].get();
					}
				}
#else
				const auto& descriptors = sun->GetRuntimeData().shadowmapDescriptors;
				g_descCount = descriptors.size();
				for (std::uint32_t i = 0; i < descriptors.size() && i < kMaxCascades; ++i) {
					cameras[i] = descriptors[i].camera.get();
					g_descCullers[i] = descriptors[i].cullingProcess;
				}
#endif
			}
			for (const auto& light : ssnData.activeShadowLights) {
				if (!light || light.get() == sun) {
					continue;
				}
#ifdef SPS_VR
				// Lage und Radius wie in SE/AE mitfuehren: ohne sie rechnete PointShadowScale in VR immer mit 1 und grosse
				// Wandschatten nah am Licht fielen dem Culling zum Opfer (bis 1.0.60)
				const auto niLight = light->light.get();
				for (const auto& desc : light->GetVRRuntimeData().shadowmapDescriptors) {
					for (const auto& cam : desc.camera) {
						if (pointCount < kMaxPointCameras && cam) {
							g_pointLightPos[pointCount] = niLight ? niLight->world.translate : RE::NiPoint3{};
							g_pointLightRadius[pointCount] = niLight ? niLight->GetLightRuntimeData().radius.x : 0.0f;
							g_pointCameras[pointCount++].store(cam.get(), std::memory_order_relaxed);
						}
					}
				}
#else
				for (const auto& desc : light->GetRuntimeData().shadowmapDescriptors) {
					if (pointCount < kMaxPointCameras && desc.camera) {
						const auto niLight = light->light.get();
						g_pointLightPos[pointCount] = niLight ? niLight->world.translate : RE::NiPoint3{};
						g_pointLightRadius[pointCount] = niLight ? niLight->GetLightRuntimeData().radius.x : 0.0f;
						g_pointCameras[pointCount++].store(desc.camera.get(), std::memory_order_relaxed);
					}
				}
#endif
			}
		}
		for (std::size_t i = 0; i < kSunCameraSlots; ++i) {
			g_sunCameras[i].store(cameras[i], std::memory_order_relaxed);
		}
		g_pointCameraCount.store(pointCount, std::memory_order_relaxed);

#ifdef SPS_VR
		if (++g_frameCounter % 600 == 0 && Config::analysis.load(std::memory_order_relaxed)) {
			logger::info("[ShadowCulling-Diag] VR: sun descriptors {} | sun cameras {} {} / {} {} | point light cameras {} | sun elevation sin={:.2f}", g_descCount,
				static_cast<const void*>(cameras[0]), static_cast<const void*>(cameras[1]), static_cast<const void*>(cameras[2]), static_cast<const void*>(cameras[3]),
				pointCount, g_sunSin.load());
			ReportCulled();
		}
		if (false) {
#else
		// Diagnose alle ~600 Frames ins Log - nur mit dem Analyse-Protokoll
		if (++g_frameCounter % 600 == 0 && Config::analysis.load(std::memory_order_relaxed)) {
#endif
			logger::info("[ShadowCulling-Diag] sun descriptors: {} | point light cameras: {} | sun elevation sin={:.2f} (~{:.0f} deg, shadow factor {:.1f})",
				g_descCount, pointCount, g_sunSin.load(), std::asin(g_sunSin.load()) * 57.2958f, 1.0f / g_sunSin.load());
			ReportCulled();
			ReportLightCacheDiag();
			LightShadowCache::Report();
			if (Config::analysis.load(std::memory_order_relaxed)) {
				ReportDecalTextures();
			}
			if (const auto mainCam = RE::Main::WorldRootCamera()) {
				logger::info("[ShadowCulling-Diag]   main camera (WorldRootCamera): {:p} '{}'", static_cast<const void*>(mainCam), mainCam->name.c_str());
			}
			if (const auto pc = RE::PlayerCamera::GetSingleton(); pc && pc->cameraRoot) {
				for (const auto& child : pc->cameraRoot->GetChildren()) {
					if (child && child->GetRTTI() && std::string_view{ child->GetRTTI()->GetName() } == "NiCamera") {
						logger::info("[ShadowCulling-Diag]   PlayerCamera child (NiCamera): {:p} '{}'", static_cast<const void*>(child.get()), child->name.c_str());
					}
				}
			}
			for (std::size_t i = 0; i < kMaxCascades && i < g_descCount; ++i) {
				logger::info("[ShadowCulling-Diag]   cascade {}: camera {:p}, culler {:p}", i, static_cast<const void*>(cameras[i]), static_cast<const void*>(g_descCullers[i]));
			}
			logger::info("[Decal-Diag] decals/frame by distance (rows) x radius (columns <25 <50 <100 <200 <500 >=500) | names: Decal {} DecalDirt {} other {} empty {}",
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
					logger::info("[ShadowCulling-Diag]   camera {:p}: {} meshes (~{}/frame)", static_cast<const void*>(cam), cnt, cnt / 600);
				}
			}
		}
	}
}
