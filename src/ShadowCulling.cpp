#include "ShadowCulling.h"
#include "Features.h"

#include "Config.h"
#include "TextureStream.h"
#include "DetourHelper.h"
#include "GpuTimer.h"
#include "LightShadowCache.h"
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
				if (const auto ref = obj->GetUserData()) {
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
			const auto count = g_pointCameraCount.load(std::memory_order_relaxed);
			const bool on = Config::analysis.load(std::memory_order_relaxed);
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
			logger::info("[Cascade-Diag]   {}: {} frames | lightTransform {} (max {:.4f}) | worldToCam {} (max {:.4f}) | frustum {} (max {:.2f}) | clipPlanes {} | splits {} (max {:.1f}) | isEnabled {} | port {}",
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
			logger::info("Cascade cache: backup texture {}x{} x{} created (target {})", desc.width, desc.height, desc.arraySize, static_cast<std::uint32_t>(a_b.target));
			return true;
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
		std::atomic<bool> g_pointCullAllowed{ true };  // false = Innenraum und dort nicht erlaubt  // Radius/Abstand zum Licht darueber -> Schatten immer behalten

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
				if (const auto ref = const_cast<RE::NiAVObject*>(obj)->GetUserData()) {
					return ref->GetFormType() == RE::FormType::ActorCharacter;
				}
			}
			return false;
		}

		bool CullActorShadow(const RE::BSGeometry& a_geom, bool a_pointLight) noexcept
		{
			const auto& cfg = Config::actorShadowCulling;
			if (!cfg.enabled || (a_pointLight && !cfg.pointLights) || !Config::masterEnabled.load(std::memory_order_relaxed) ||
				!const_cast<RE::BSGeometry&>(a_geom).GetGeometryRuntimeData().skinInstance) {
				return false;
			}
			if (DistanceToCamera(a_geom.worldBound) > cfg.minDistance && BelongsToActor(a_geom)) {
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
					if (g_cacheSkip.load(std::memory_order_relaxed) && Config::cascadeCache.skipDraws && cascade >= Config::cascadeCache.cascade) {
						Stats::Count(Stats::Counter::CascadeSkipped);
						return;  // Kaskade kommt diesen Frame aus dem Cache
					}
					if (!a_visible.GetGeometryRuntimeData().skinInstance && ShadowOutsideView(a_visible.worldBound)) {
						Stats::Count(Stats::Counter::SunViewCulled);
						return;
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
					if (g_pointCullAllowed.load(std::memory_order_relaxed) && !NearPointLight(a_this->camera, a_visible) && ShouldCull(Config::pointLightCulling, a_visible, UINT32_MAX)) {
						Stats::Count(Stats::Counter::PointCulled);
						return;
					}
					Stats::Count(Stats::Counter::PointKept);
					RecordPointMesh(a_this->camera, a_visible);
					LightShadowCache::NotCacheable(a_this->camera);  // dieser Culler trennt Schatten und Beleuchtung nicht
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
				GpuTimer::Begin(GpuTimer::kDepthPrepass);
				func(a_arg1, a_arg2);
				GpuTimer::End(GpuTimer::kDepthPrepass);
				g_inDepthPrepass.store(false, std::memory_order_relaxed);
			}
			static inline void (*func)(bool, bool) = nullptr;
		};

		// Eigene vtable von BSParabolicCullingProcess (Punktlicht-Schatten): der Eintrag 0x18 zeigt direkt auf die
		// Basis-Implementierung und laeuft daher NICHT ueber den Hook auf BSCullingProcess. Alles hier ist Punktlicht.
		struct AppendVirtualParabolic
		{
			// Dieser Culler sammelt in einem Durchlauf zweierlei, gesteuert ueber den Modus an +0x301F4 (in CommonLib
			// "alphaGroupStopIndex"; AppendNonAccum ID 108604): Bit 0 = das Licht beleuchtet das Mesh (Licht-Zuordnung),
			// Bit 1 = das Mesh wirft Schatten. Weglassen darf nur den Schatten treffen - sonst fehlt das Licht auf dem Mesh.
			// Bis 1.0.32 fiel beides weg: im Schatten-Cache ging das Licht auf Waenden und Boden aus (Community Shaders
			// wie Vanilla), beim Aufbau kurz auf Figuren. Nur AE geprueft; SE/VR wie bisher alles weglassen.
			static void SkipShadow(RE::BSCullingProcess* a_this, RE::BSGeometry& a_visible, std::int32_t a_alphaGroupIndex)
			{
				if (!REL::Module::IsAE()) {
					return;
				}
				const auto mode = static_cast<std::int32_t>(a_this->alphaGroupStopIndex);
				if ((mode & 1) == 0) {
					return;  // reiner Schatten-Durchlauf
				}
				if (a_alphaGroupIndex != -1 || a_this->isGroupingAlphas) {
					Stats::Count(Stats::Counter::PointLitOnlyGrouped);
					func(a_this, a_visible, a_alphaGroupIndex);  // Alpha-Gruppen (selten): unveraendert, Schatten bleibt
					return;
				}
				Stats::Count(Stats::Counter::PointLitOnly);
				a_this->AppendNonAccum(a_visible, mode & ~2);  // nur Beleuchtung eintragen
			}

			static void thunk(RE::BSCullingProcess* a_this, RE::BSGeometry& a_visible, std::int32_t a_alphaGroupIndex)
			{
				DiagRecord(a_this->camera);
				if (g_pointCullAllowed.load(std::memory_order_relaxed) && !NearPointLight(a_this->camera, a_visible) && ShouldCull(Config::pointLightCulling, a_visible, UINT32_MAX)) {
					Stats::Count(Stats::Counter::PointCulled);
					SkipShadow(a_this, a_visible, a_alphaGroupIndex);
					return;
				}
				Stats::Count(Stats::Counter::PointKept);
				RecordPointMesh(a_this->camera, a_visible);
				if (LightShadowCache::FilterAppend(a_this->camera, a_visible)) {
					SkipShadow(a_this, a_visible, a_alphaGroupIndex);  // Schatten steckt in der zwischengespeicherten Karte
					return;
				}
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

		// ---- Teilbaeume ueberspringen (Process = vfunc 0x16, wird fuer jeden Knoten/jedes Objekt im Durchlauf aufgerufen) ----
		thread_local int g_insideActor = 0;  // Figuren nie ueberspringen (ihre Schatten haben eigene Regeln)

		bool IsActorRoot(RE::NiAVObject* a_obj) noexcept
		{
			const auto ref = a_obj->GetUserData();
			return ref && (ref->GetFormType() == RE::FormType::ActorCharacter);
		}

		// Kamera der Niederschlags-/Skylighting-Verdeckungskarte. Erkennung ueber die Kamera, nicht ueber den Culler:
		// das Spiel verteilt den Durchlauf auf Kopien des Cullers (Zeigervergleich traf nie).
		const RE::NiCamera* PrecipCamera() noexcept
		{
			const auto sky = RE::Sky::GetSingleton();
			return sky && sky->precip ? sky->precip->occlusionData.camera.get() : nullptr;
		}

		// true = Knoten samt Inhalt ueberspringen
		bool ShouldPrune(RE::BSCullingProcess* a_this, RE::NiAVObject* a_obj, bool a_parabolic) noexcept
		{
			const auto node = a_obj ? a_obj->AsNode() : nullptr;
			if (!node || g_insideActor > 0 || !Config::subtreePruning.enabled || !Config::masterEnabled.load(std::memory_order_relaxed)) {
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
				if (g_cacheSkip.load(std::memory_order_relaxed) && Config::cascadeCache.skipDraws && cascade >= Config::cascadeCache.cascade) {
					return false;  // Kaskaden-Cache entscheidet selbst
				}
				if (ShadowOutsideView(bound)) {
					Stats::Count(Stats::Counter::PrunedSunView);
					return true;
				}
				if (sp.sun && g_sunCullAllowed.load(std::memory_order_relaxed) &&
					ShouldCullBound(Config::shadowCulling, bound, cascade, 1.0f / g_sunSin.load(std::memory_order_relaxed))) {
					Stats::Count(Stats::Counter::PrunedSun);
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

		template <int N, bool Parabolic>
		struct Process
		{
			static void thunk(RE::BSCullingProcess* a_this, RE::NiAVObject* a_obj, std::uint32_t a_arg)
			{
				if (a_obj && IsActorRoot(a_obj)) {
					++g_insideActor;
					func(a_this, a_obj, a_arg);
					--g_insideActor;
					return;
				}
				if (ShouldPrune(a_this, a_obj, Parabolic)) {
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

	void SetInSunAccumulate(bool a_in) noexcept
	{
		g_inSunAccumulate = a_in;
	}

	namespace
	{
		// ---------------- Stabile ferne Kaskade ----------------
		// Die Engine richtet jede Kaskade in jedem Frame neu aus (UpdateCamera, AE ID 108496): Lage folgt der Sonne,
		// Ausschnitt (Ortho-Frustum l/r/t/b) dem Blick, Tiefenbereich den Objekten. Fuer die ferne Kaskade halten wir
		// stattdessen einen Bezugsrahmen (Drehung + Position) und einen Ausschnitt fest, der den Engine-Ausschnitt
		// umschliesst, und richten nur neu aus, wenn er nicht mehr passt. Kamera-Achsen: Spalte 0 = Blickrichtung,
		// left/right laufen entlang -Spalte 2, top/bottom entlang -Spalte 1 (gespiegelt, per worldToCam nachgemessen:
		// Kameraposition lag bei clip x = +0,81 statt -0,81). Mit +Spalten wanderte der Ausschnitt beim Gehen doppelt so
		// weit in die falsche Richtung -> fehlende Schatten (Test 1.0.10). Engine-Weg: SetViewFrustum (70626) + Update (70251).
		struct StableState
		{
			bool          valid = false;
			RE::NiMatrix3 rot{};
			RE::NiPoint3  pos{};
			float         cx = 0.0f, cy = 0.0f, hx = 0.0f, hy = 0.0f, n = 0.0f, f = 0.0f;
		};
		StableState   g_stable{};
		std::uint32_t g_stableFrames = 0, g_stableChanged = 0, g_stableReanchor = 0;

		RE::NiPoint3 Col(const RE::NiMatrix3& a_m, int a_c) noexcept { return { a_m.entry[0][a_c], a_m.entry[1][a_c], a_m.entry[2][a_c] }; }
		float        Dot3(const RE::NiPoint3& a_a, const RE::NiPoint3& a_b) noexcept { return a_a.x * a_b.x + a_a.y * a_b.y + a_a.z * a_b.z; }
		float        QuantUp(float a_v, float a_step) noexcept { return std::ceil(a_v / a_step) * a_step; }
		float        QuantDown(float a_v, float a_step) noexcept { return std::floor(a_v / a_step) * a_step; }

		void StabilizeFarCascade(RE::BSShadowDirectionalLight* a_light) noexcept
		{
			const auto& cfg = Config::stableCascade;
			if (!cfg.enabled || !Config::masterEnabled.load(std::memory_order_relaxed)) {
				g_stable.valid = false;
				return;
			}
			auto&      descs = a_light->GetRuntimeData().shadowmapDescriptors;
			const auto idx = Config::cascadeCache.cascade;
			if (idx >= descs.size()) {
				return;
			}
			auto&      desc = descs[idx];
			const auto cam = desc.camera.get();
			if (!cam) {
				return;
			}
			auto& fr = cam->GetRuntimeData2().viewFrustum;
			if (!fr.bOrtho || fr.fRight <= fr.fLeft || fr.fTop <= fr.fBottom) {
				return;
			}
			const RE::NiMatrix3 rotE = cam->world.rotate;
			const RE::NiPoint3  posE = cam->world.translate;
			const RE::NiPoint3  dirE = Col(rotE, 0), upE = -Col(rotE, 1), rightE = -Col(rotE, 2);
			// Engine-Ausschnitt: Mitte als Weltpunkt, halbe Breite/Hoehe, Tiefenbereich
			const float         exc = 0.5f * (fr.fLeft + fr.fRight), eyc = 0.5f * (fr.fTop + fr.fBottom);
			const float         ehx = 0.5f * (fr.fRight - fr.fLeft), ehy = 0.5f * (fr.fTop - fr.fBottom);
			const RE::NiPoint3  centerW = posE + rightE * exc + upE * eyc;

			const int mode = static_cast<int>(std::lround(cfg.debugMode));
			if (mode == 2) {
				// Diagnose: Engine-Werte unveraendert ueber unseren Weg (local setzen + Update) - prueft den Mechanismus
				cam->local.rotate = rotE;
				cam->local.translate = posE;
				RE::NiUpdateData ud2{};
				cam->Update(ud2);
				return;
			}
			auto& s = g_stable;
			++g_stableFrames;
			bool changed = false;
			bool reanchor = !s.valid || mode == 5;
			if (s.valid) {
				reanchor = Dot3(dirE, Col(s.rot, 0)) < std::cos(cfg.maxAngleDeg * 0.017453292f);
			}
			if (reanchor) {
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
			// Tiefe: Engine-Bereich muss hineinpassen, nicht unnoetig gross
			if (mode == 4) {
				s.n = en;
				s.f = ef;
			} else if (mode == 6 || mode == 7) {
				// Diagnose: eine Ebene wie die Engine, die andere festgehalten
				const float heldN = std::max(1.0f, QuantDown(en - 0.25f * cfg.depthStep, cfg.depthStep));
				const float heldF = QuantUp(ef + 0.25f * cfg.depthStep, cfg.depthStep);
				if (changed || s.n == 0.0f || s.f == 0.0f || en < s.n || ef > s.f) {
					s.n = heldN;
					s.f = heldF;
				}
				if (mode == 6) {
					s.n = en;  // nah (zur Sonne) wie Engine, fern festgehalten
				} else {
					s.f = ef;  // fern wie Engine, nah festgehalten
				}
			} else if (changed || en < s.n || ef > s.f || (s.f - s.n) > (ef - en) + 4.0f * cfg.depthStep) {
				// eng an der Engine: nur depthStep Spielraum je Seite (Zittern im Stand), sonst sofort nachfuehren
				s.n = std::max(1.0f, en - cfg.depthStep);
				s.f = ef + cfg.depthStep;
				changed = true;
			}
			if (changed) {
				++g_stableChanged;
			}

			if (mode == 1) {
				return;  // Diagnose: nur rechnen
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
			if (mode != 3) {
				RE::NiUpdateData ud{};
				cam->Update(ud);
			}
			// Mit Kaskaden-Cache: Schattenwerfer fuer den GANZEN gehaltenen Ausschnitt sammeln. Die Engine cullt sonst mit
			// einer engen Huelle um den aktuellen Sichtbereich (customCullPlanes) - beim Drehen fehlten im wiederverwendeten
			// Frame Schatten am Rand. Ohne die Huelle gelten die Ebenen der Kamera, also unser Ausschnitt.
			if (Config::cascadeCache.enabled && desc.cullingProcess) {
				desc.cullingProcess->doCustomCullPlanes = false;
			}

			static auto lastLog = std::chrono::steady_clock::now();
			if (std::chrono::steady_clock::now() - lastLog >= std::chrono::seconds(10)) {
				lastLog = std::chrono::steady_clock::now();
				logger::info("[StableCascade] {} frames | changed {} | sun re-anchored {} | window {:.0f} x {:.0f} (engine {:.0f} x {:.0f}) | depth {:.0f}-{:.0f} (engine {:.0f}-{:.0f})",
					g_stableFrames, g_stableChanged, g_stableReanchor, 2.0f * s.hx, 2.0f * s.hy, 2.0f * ehx, 2.0f * ehy, s.n, s.f, en, ef);
				g_stableFrames = g_stableChanged = g_stableReanchor = 0;
			}
		}
	}

	void AfterSunUpdateCamera(RE::BSShadowDirectionalLight* a_light) noexcept
	{
		if (!a_light) {
			return;
		}
		StabilizeFarCascade(a_light);
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
		UpdateViewPlanes(a_light);
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

	// Diagnose (Menue-Knopf): wie richtet die Engine die Kaskaden pro Frame aus? Je Frame und Kaskade eine Zeile mit
	// Frustum (Tiefenbereich!), Kamera-Lage, unitsPerTexel und lightTransform - Grundlage fuer eine stabile ferne Kaskade.
	std::atomic<int> g_cascadeDump{ 0 };
	std::uint32_t    g_cascadeDumpFrame = 0;

	void RequestCascadeDump(int a_frames) noexcept
	{
		g_cascadeDumpFrame = 0;
		g_cascadeDump.store(a_frames, std::memory_order_relaxed);
	}

	namespace
	{
		void DumpCascades(RE::BSShadowDirectionalLight* a_light) noexcept
		{
			const auto frame = g_cascadeDumpFrame++;
			if (frame == 0) {
				logger::info("[Cascade-Dump] start (frame | cascade | frustum l r t b near far ortho | cam pos | cam dir | unitsPerTexel | port | lightTransform rows)");
			}
			if (const auto main = RE::Main::WorldRootCamera()) {
				const auto& w = main->world;
				const auto& dir = a_light->GetShadowDirectionalLightRuntimeData();
				logger::info("[Cascade-Dump] {} main pos {:.1f} {:.1f} {:.1f} fwd {:.4f} {:.4f} {:.4f} | sun {:.5f} {:.5f} {:.5f} | splits {:.0f}-{:.0f} {:.0f}-{:.0f}",
					frame, w.translate.x, w.translate.y, w.translate.z, w.rotate.entry[0][0], w.rotate.entry[1][0], w.rotate.entry[2][0],
					dir.sunVector.x, dir.sunVector.y, dir.sunVector.z, dir.startSplitDistances[0], dir.endSplitDistances[0], dir.startSplitDistances[1], dir.endSplitDistances[1]);
			}
			auto& descs = a_light->GetRuntimeData().shadowmapDescriptors;
			for (std::uint32_t i = 0; i < descs.size() && i < kMaxCascades; ++i) {
				const auto& d = descs[i];
				const auto  cam = d.camera.get();
				if (!cam) {
					continue;
				}
				const auto& f = cam->GetRuntimeData2().viewFrustum;
				const auto& w = cam->world;
				const auto& m = d.lightTransform.m;
				const auto* port = reinterpret_cast<const std::int32_t*>(&d.port);  // left right top bottom (Felder protected)
				logger::info("[Cascade-Dump] {} C{} | {:.2f} {:.2f} {:.2f} {:.2f} {:.1f} {:.1f} {} | {:.1f} {:.1f} {:.1f} | {:.5f} {:.5f} {:.5f} | {} | {} {} {} {} | "
							 "{:.6f} {:.6f} {:.6f} {:.3f} / {:.6f} {:.6f} {:.6f} {:.3f} / {:.6f} {:.6f} {:.6f} {:.3f} / {:.6f} {:.6f} {:.6f} {:.3f}",
					frame, i, f.fLeft, f.fRight, f.fTop, f.fBottom, f.fNear, f.fFar, f.bOrtho, w.translate.x, w.translate.y, w.translate.z,
					w.rotate.entry[0][0], w.rotate.entry[1][0], w.rotate.entry[2][0], d.unitsPerTexel, port[0], port[1], port[2], port[3],
					m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1], m[1][2], m[1][3], m[2][0], m[2][1], m[2][2], m[2][3], m[3][0], m[3][1], m[3][2], m[3][3]);
				// Volle Drehung (Spalten 0/1/2) und worldToCam: Achsen-Zuordnung von left/right bzw. top/bottom pruefen
				const auto& r = w.rotate.entry;
				const auto& wc = cam->GetRuntimeData().worldToCam;
				logger::info("[Cascade-Dump] {} C{} rot | {:.5f} {:.5f} {:.5f} / {:.5f} {:.5f} {:.5f} / {:.5f} {:.5f} {:.5f} | w2c {:.7f} {:.7f} {:.7f} {:.4f} / {:.7f} {:.7f} {:.7f} {:.4f} / {:.7f} {:.7f} {:.7f} {:.4f} / {:.7f} {:.7f} {:.7f} {:.4f}",
					frame, i, r[0][0], r[0][1], r[0][2], r[1][0], r[1][1], r[1][2], r[2][0], r[2][1], r[2][2],
					wc[0][0], wc[0][1], wc[0][2], wc[0][3], wc[1][0], wc[1][1], wc[1][2], wc[1][3], wc[2][0], wc[2][1], wc[2][2], wc[2][3], wc[3][0], wc[3][1], wc[3][2], wc[3][3]);
			}
			if (g_cascadeDump.fetch_sub(1, std::memory_order_relaxed) == 1) {
				logger::info("[Cascade-Dump] end");
			}
		}
	}

	// Mit stabiler Kaskade ist die Projektion im Cache-Frame dieselbe - die Engine-Matrix des aktuellen Frames passt dann
	// zur alten Karte. Die alte Matrix einzusetzen waere falsch: sie enthaelt den Kamera-Versatz des Vorframes
	// (kamera-relative Darstellung) -> Schatten zuckten beim Bewegen in jedem 2. Frame (Test 1.0.10).
	bool FreezeMatrix() noexcept
	{
		return Config::cascadeCache.freezeMatrix && !Config::stableCascade.enabled;
	}

	void AfterSunRender() noexcept
	{
		if (g_cacheLight && g_cascadeDump.load(std::memory_order_relaxed) > 0) {
			DumpCascades(g_cacheLight);
		}
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
				if (skipNow && FreezeMatrix() && c.finalValid) {
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
			logger::error("Cascade cache: backup texture could not be created - cache disabled");
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
			if (skip && FreezeMatrix() && c.finalValid) {
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
				if (FreezeMatrix()) {
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
		TextureStream::OnFrame();
		g_viewValid.store(false, std::memory_order_relaxed);  // neu erst beim naechsten Sonnen-Accumulate (Innenraeume: nie)
#ifndef SPS_VR
		FinishLightCacheDiag();  // vor dem Neuaufbau der Kameraliste: Zaehler gehoeren zur Liste des letzten Frames
		LightShadowCache::OnFrame();
#endif

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
				for (const auto& desc : light->GetVRRuntimeData().shadowmapDescriptors) {
					for (const auto& cam : desc.camera) {
						if (pointCount < kMaxPointCameras && cam) {
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
			// Kaskaden-Cache: Ziel-Textur und Slice aller Schattenkarten (Sonne + Punktlichter) - teilen sie sich etwas?
			if (const auto ssn = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0]) {
				auto& ssnData = ssn->GetRuntimeData();
				if (const auto sun = ssnData.sunShadowDirLight) {
					const auto& descs = sun->GetRuntimeData().shadowmapDescriptors;
					for (std::uint32_t i = 0; i < descs.size(); ++i) {
						const auto& d = descs[i];
						logger::info("[Cascade-Diag]   sun C{}: target {} slice {} clear {} active {} port {}x{}", i, static_cast<std::uint32_t>(d.renderTarget), d.shadowmapIndex,
							d.clearRenderTarget, d.isEnabled, d.port.GetWidth(), d.port.GetHeight());
					}
				}
				std::uint32_t n = 0;
				for (const auto& light : ssnData.activeShadowLights) {
					if (!light || light.get() == ssnData.sunShadowDirLight || n >= 6) {
						continue;
					}
					for (const auto& d : light->GetRuntimeData().shadowmapDescriptors) {
						logger::info("[Cascade-Diag]   light {}: target {} slice {}", n, static_cast<std::uint32_t>(d.renderTarget), d.shadowmapIndex);
					}
					++n;
				}
			}
			ReportCulled();
			ReportLightCacheDiag();
			LightShadowCache::Report();
			if (Config::analysis.load(std::memory_order_relaxed)) {
				ReportDecalTextures();
			}
			logger::info("[Cascade-Diag]   cache invalidations (target/slice changed): {} | normal frames far cascade clearRenderTarget false/true: {}/{}",
				g_cacheInvalidations, g_clearFlagSeen[0], g_clearFlagSeen[1]);
			g_clearFlagSeen = {};
			logger::info("[Cascade-Diag]   Render changes lightTransform after Accumulate: {}/{} (max {:.4f})", g_renderChangedMatrix, g_renderChecked, g_renderChangedMax);
			logger::info("[Cascade-Diag]   projection same as in last drawn frame: {}/{} frames", g_projSame, g_projChecked);
			g_projSame = g_projChecked = 0;
			g_renderChangedMatrix = g_renderChecked = 0;
			g_renderChangedMax = 0.0f;
			LogDiff("cache frame vs. last drawn", g_diffCache);
			LogDiff("drawn vs. previous drawn", g_diffNormal);
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
