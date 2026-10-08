#include "LightShadowCache.h"

#include "Config.h"
#include "Features.h"
#include "Stats.h"

#include <unordered_set>

namespace LightShadowCache
{
	namespace
	{
		enum class Mode : int
		{
			kNormal,  // wie die Engine: alles zeichnen
			kBuild,   // nur Unbewegliche zeichnen, danach in den Cache kopieren
			kCached   // Cache zurueckkopieren, nur Bewegliche zeichnen
		};

		constexpr std::uint32_t kMaxMaps = 32;       // gleichzeitig verwaltete Schattenkarten
		constexpr std::uint32_t kStableFrames = 3;   // so lange muss ein Licht ruhen, bevor aufgebaut wird
		constexpr float         kMoveEpsilon = 0.5f; // Einheiten - darueber gilt das Licht als bewegt (Flackern)

		struct MapState
		{
			std::atomic<const RE::NiCamera*> camera{ nullptr };
			std::atomic<int>                 mode{ static_cast<int>(Mode::kNormal) };
			// Main-Thread
			RE::NiTransform            pose{};
			float                      radius = 0;
			std::uint32_t              stableFrames = 0;
			std::uint32_t              lastSeen = 0;
			bool                       valid = false;   // Cache-Inhalt passt
			bool                       drawn = false;   // in diesem Frame gezeichnet (Zeichen-Aufruf gesehen)
			REX::W32::ID3D11Texture2D* tex = nullptr;   // eigene Kopie einer Ebene
			std::uint32_t              texW = 0, texH = 0, texFormat = 0;
			// Culling-Jobs (unter lock)
			std::mutex                                               lock;
			struct Entry
			{
				std::uint64_t hash;
				std::uint32_t seen;  // Frame der letzten Pruefung (doppelte Aufnahme nur einmal zaehlen)
			};
			std::unordered_map<const RE::BSGeometry*, Entry> statics;  // Inhalt des Caches
			std::unordered_set<const RE::BSGeometry*>                promoted;  // gelernte Bewegliche
			std::uint32_t                                            matched = 0;
			bool                                                     broken = false;  // Abweichung -> neu aufbauen
		};
		std::array<MapState, kMaxMaps> g_maps;
		std::atomic<std::uint32_t>     g_frame{ 0 };
		bool                           g_installed = false;

		struct Window
		{
			std::uint64_t frames = 0, cached = 0, builds = 0, normal = 0;
			std::uint64_t lightMoved = 0, invAdded = 0, invRemoved = 0, invPromoted = 0, buildFailed = 0;
		} g_win;
		std::atomic<std::uint64_t> g_saved{ 0 }, g_drawnDynamic{ 0 }, g_invAddedJobs{ 0 }, g_invPromotedJobs{ 0 };

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

		std::uint64_t PoseHash(const RE::NiTransform& a_w) noexcept
		{
			std::uint64_t h = Mix64(Quant(a_w.translate.x, 4.0f));
			h = Mix64(h ^ Quant(a_w.translate.y, 4.0f));
			h = Mix64(h ^ Quant(a_w.translate.z, 4.0f));
			for (int r = 0; r < 3; ++r) {
				for (int c = 0; c < 3; ++c) {
					h = Mix64(h ^ Quant(a_w.rotate.entry[r][c], 1000.0f));
				}
			}
			return Mix64(h ^ Quant(a_w.scale, 1000.0f));
		}

		// Figuren und alles, was an ihnen haengt (Waffen, Schilde, Pfeile, getragene Gegenstaende), sowie Effekte
		bool IsDynamic(RE::BSGeometry& a_geom) noexcept
		{
			if (a_geom.GetGeometryRuntimeData().skinInstance) {
				return true;
			}
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

		MapState* Find(const RE::NiCamera* a_camera) noexcept
		{
			for (auto& m : g_maps) {
				if (m.camera.load(std::memory_order_relaxed) == a_camera) {
					return &m;
				}
			}
			return nullptr;
		}

		bool PoseChanged(const RE::NiTransform& a_a, const RE::NiTransform& a_b) noexcept
		{
			const float dx = a_a.translate.x - a_b.translate.x, dy = a_a.translate.y - a_b.translate.y, dz = a_a.translate.z - a_b.translate.z;
			if (dx * dx + dy * dy + dz * dz > kMoveEpsilon * kMoveEpsilon) {
				return true;
			}
			for (int r = 0; r < 3; ++r) {
				for (int c = 0; c < 3; ++c) {
					if (std::abs(a_a.rotate.entry[r][c] - a_b.rotate.entry[r][c]) > 1e-4f) {
						return true;
					}
				}
			}
			return false;
		}

		void Drop(MapState& a_m) noexcept
		{
			if (a_m.tex) {
				a_m.tex->Release();
				a_m.tex = nullptr;
			}
			{
				std::scoped_lock lock(a_m.lock);
				a_m.statics.clear();
				a_m.promoted.clear();
				a_m.matched = 0;
				a_m.broken = false;
			}
			a_m.valid = false;
			a_m.drawn = false;
			a_m.stableFrames = 0;
			a_m.mode.store(static_cast<int>(Mode::kNormal), std::memory_order_relaxed);
			a_m.camera.store(nullptr, std::memory_order_relaxed);
		}

		// Deskriptor zur Kamera in den aktiven Schattenlichtern (Main-Thread; Platz/Ziel gelten erst nach der Zuteilung)
		const RE::BSShadowLight::ShadowmapDescriptor* FindDescriptor(const RE::NiCamera* a_camera) noexcept
		{
			const auto ssn = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
			if (!ssn) {
				return nullptr;
			}
			for (const auto& light : ssn->GetRuntimeData().activeShadowLights) {
				if (!light) {
					continue;
				}
				for (const auto& d : light->GetRuntimeData().shadowmapDescriptors) {
					if (d.camera.get() == a_camera) {
						return &d;
					}
				}
			}
			return nullptr;
		}

		// Ziel-Textur und Unterressource der Schattenkarte; Cache-Textur passend anlegen
		REX::W32::ID3D11Texture2D* TargetTexture(const RE::BSShadowLight::ShadowmapDescriptor& a_d, std::uint32_t& a_sub) noexcept
		{
			const auto renderer = RE::BSGraphics::Renderer::GetSingleton();
			const auto target = static_cast<std::uint32_t>(a_d.renderTarget);
			if (!renderer || target >= RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL) {
				return nullptr;
			}
			const auto tex = renderer->GetDepthStencilData().depthStencils[target].texture;
			if (!tex) {
				return nullptr;
			}
			REX::W32::D3D11_TEXTURE2D_DESC desc{};
			tex->GetDesc(&desc);
			if (a_d.shadowmapIndex >= desc.arraySize || desc.sampleDesc.count != 1) {
				return nullptr;
			}
			a_sub = a_d.shadowmapIndex * desc.mipLevels;
			return tex;
		}

		bool EnsureCacheTexture(MapState& a_m, REX::W32::ID3D11Texture2D* a_src) noexcept
		{
			REX::W32::D3D11_TEXTURE2D_DESC desc{};
			a_src->GetDesc(&desc);
			if (a_m.tex && a_m.texW == desc.width && a_m.texH == desc.height && a_m.texFormat == static_cast<std::uint32_t>(desc.format)) {
				return true;
			}
			if (a_m.tex) {
				a_m.tex->Release();
				a_m.tex = nullptr;
			}
			desc.arraySize = 1;
			desc.mipLevels = 1;
			desc.usage = REX::W32::D3D11_USAGE_DEFAULT;
			desc.bindFlags = 0;
			desc.cpuAccessFlags = 0;
			desc.miscFlags = 0;
			const auto device = RE::BSGraphics::Renderer::GetDevice();
			if (!device || device->CreateTexture2D(&desc, nullptr, &a_m.tex) < 0 || !a_m.tex) {
				a_m.tex = nullptr;
				return false;
			}
			a_m.texW = desc.width;
			a_m.texH = desc.height;
			a_m.texFormat = static_cast<std::uint32_t>(desc.format);
			return true;
		}

		// Kopieren bei abgehaengten Render-Zielen (die Schattenkarte ist gerade als Tiefenpuffer gebunden)
		void CopyUnbound(REX::W32::ID3D11Resource* a_dst, std::uint32_t a_dstSub, REX::W32::ID3D11Resource* a_src, std::uint32_t a_srcSub) noexcept
		{
			const auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
			REX::W32::ID3D11RenderTargetView* rtvs[8]{};
			REX::W32::ID3D11DepthStencilView* dsv = nullptr;
			context->OMGetRenderTargets(8, rtvs, &dsv);
			context->OMSetRenderTargets(0, nullptr, nullptr);
			context->CopySubresourceRegion(a_dst, a_dstSub, 0, 0, 0, a_src, a_srcSub, nullptr);
			context->OMSetRenderTargets(8, rtvs, dsv);
			for (auto& v : rtvs) {
				if (v) {
					v->Release();
				}
			}
			if (dsv) {
				dsv->Release();
			}
		}

		// ID 107604 (+0x167) ruft hier das Zeichnen einer Schattenkarte auf (Kamera, Shader-Akkumulator, Flags)
		struct DrawShadowmap
		{
			static void thunk(RE::NiCamera* a_camera, void* a_accumulator, std::uint32_t a_flags)
			{
				MapState* m = a_camera ? Find(a_camera) : nullptr;
				const auto mode = m ? static_cast<Mode>(m->mode.load(std::memory_order_relaxed)) : Mode::kNormal;
				if (mode == Mode::kNormal) {
					func(a_camera, a_accumulator, a_flags);
					return;
				}
				const auto    desc = FindDescriptor(a_camera);
				std::uint32_t sub = 0;
				const auto    target = desc ? TargetTexture(*desc, sub) : nullptr;
				if (!target || !EnsureCacheTexture(*m, target)) {
					// Ohne Ziel kein Cache: dieser Frame zeichnet nur einen Teil -> verwerfen und neu aufbauen
					func(a_camera, a_accumulator, a_flags);
					m->valid = false;
					++g_win.buildFailed;
					return;
				}
				m->drawn = true;
				if (mode == Mode::kCached) {
					CopyUnbound(target, sub, m->tex, 0);  // unbewegliche Schatten zurueck
					func(a_camera, a_accumulator, a_flags);  // nur Bewegliche
				} else {
					func(a_camera, a_accumulator, a_flags);  // nur Unbewegliche
					CopyUnbound(m->tex, 0, target, sub);
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		if (!REL::Module::IsAE()) {
			Features::Report("Shadow cache for static lights", "Schatten fester Lichter zwischenspeichern", false, "Anniversary Edition only", true);
			return;
		}
		const REL::Relocation<std::uintptr_t> site{ REL::ID(107604), 0x167 };
		const REL::Relocation<std::uintptr_t> target{ REL::ID(106436) };
		if (!Features::IsCall(site.address(), target.address())) {
			Features::Report("Shadow cache for static lights", "Schatten fester Lichter zwischenspeichern", false, "draw call site ID 107604+0x167 differs");
			return;
		}
		DrawShadowmap::func = SKSE::GetTrampoline().write_call<5>(site.address(), DrawShadowmap::thunk);
		g_installed = true;
		Features::Report("Shadow cache for static lights", "Schatten fester Lichter zwischenspeichern", true);
		logger::info("Hook installed: shadow map draw (ID 107604+0x167) - shadow cache for static lights");
	}

	bool FilterAppend(const RE::NiCamera* a_camera, RE::BSGeometry& a_geom) noexcept
	{
		if (!g_installed) {
			return false;
		}
		MapState* m = Find(a_camera);
		if (!m) {
			return false;
		}
		const auto mode = static_cast<Mode>(m->mode.load(std::memory_order_relaxed));
		if (mode == Mode::kNormal) {
			return false;
		}
		bool dynamic = IsDynamic(a_geom);
		try {
			std::scoped_lock lock(m->lock);
			dynamic = dynamic || m->promoted.contains(&a_geom);
			if (mode == Mode::kBuild) {
				if (dynamic) {
					return true;  // Aufbau-Frame: nur Unbewegliche in die Karte (Bewegliche fehlen einen Frame)
				}
				m->statics[&a_geom] = { PoseHash(a_geom.world), 0 };
				return false;
			}
			// Cache-Frame
			if (dynamic) {
				g_drawnDynamic.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			const auto it = m->statics.find(&a_geom);
			if (it == m->statics.end()) {
				m->broken = true;  // neu im Lichtradius: diesen Frame zusaetzlich zeichnen, dann neu aufbauen
				g_invAddedJobs.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			const auto frame = g_frame.load(std::memory_order_relaxed);
			if (it->second.seen != frame) {
				it->second.seen = frame;
				++m->matched;
			}
			if (it->second.hash != PoseHash(a_geom.world)) {
				m->promoted.insert(&a_geom);  // bewegt sich -> ab jetzt immer neu zeichnen
				m->broken = true;
				g_invPromotedJobs.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
		} catch (...) {
			return false;
		}
		g_saved.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	void OnFrame()
	{
		if (!g_installed) {
			return;
		}
		++g_frame;
		const bool enabled = Config::lightShadowCache.enabled && Config::masterEnabled.load(std::memory_order_relaxed);

		// 1. letzten Frame auswerten
		for (auto& m : g_maps) {
			if (!m.camera.load(std::memory_order_relaxed)) {
				continue;
			}
			const auto mode = static_cast<Mode>(m.mode.load(std::memory_order_relaxed));
			if (mode == Mode::kNormal || !m.drawn) {
				m.drawn = false;
				continue;
			}
			m.drawn = false;
			std::scoped_lock lock(m.lock);
			if (mode == Mode::kBuild) {
				m.valid = !m.statics.empty();
				++g_win.builds;
			} else {
				++g_win.cached;
				if (m.broken || m.matched != m.statics.size()) {
					if (!m.broken) {
						++g_win.invRemoved;  // ein Unbewegliches fehlt (weggenommen, Tuer geoeffnet ...)
					}
					m.valid = false;
				}
			}
			m.broken = false;
			m.matched = 0;
		}

		// 2. Modus je Schattenkarte fuer diesen Frame
		const auto ssn = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		if (ssn) {
			auto& data = ssn->GetRuntimeData();
			for (const auto& light : data.activeShadowLights) {
				if (!light || light.get() == data.sunShadowDirLight) {
					continue;
				}
				const auto niLight = light->light.get();
				const float radius = niLight ? niLight->GetLightRuntimeData().radius.x : 0.0f;
				for (const auto& d : light->GetRuntimeData().shadowmapDescriptors) {
					const auto cam = d.camera.get();
					if (!cam) {
						continue;
					}
					MapState* m = Find(cam);
					if (!m) {
						for (auto& slot : g_maps) {
							if (!slot.camera.load(std::memory_order_relaxed)) {
								slot.pose = cam->world;
								slot.radius = radius;
								slot.stableFrames = 0;
								slot.valid = false;
								slot.camera.store(cam, std::memory_order_relaxed);
								m = &slot;
								break;
							}
						}
						if (!m) {
							continue;  // voll: dieses Licht normal zeichnen
						}
					}
					m->lastSeen = g_frame.load(std::memory_order_relaxed);
					if (PoseChanged(cam->world, m->pose) || std::abs(radius - m->radius) > kMoveEpsilon) {
						if (m->valid) {
							++g_win.lightMoved;
						}
						m->pose = cam->world;
						m->radius = radius;
						m->stableFrames = 0;
						m->valid = false;
					} else {
						++m->stableFrames;
					}
					Mode mode = Mode::kNormal;
					if (enabled && m->valid) {
						mode = Mode::kCached;
					} else if (enabled && m->stableFrames >= kStableFrames) {
						mode = Mode::kBuild;
						std::scoped_lock lock(m->lock);
						m->statics.clear();
					}
					if (mode == Mode::kNormal) {
						++g_win.normal;
					}
					m->mode.store(static_cast<int>(mode), std::memory_order_relaxed);
				}
			}
		}
		// Karten, die gerade nicht aktiv sind: normal; lange nicht gesehen -> freigeben
		for (auto& m : g_maps) {
			const auto frame = g_frame.load(std::memory_order_relaxed);
			if (m.camera.load(std::memory_order_relaxed) && m.lastSeen != frame) {
				m.mode.store(static_cast<int>(Mode::kNormal), std::memory_order_relaxed);
				if (frame - m.lastSeen > 600 || !enabled) {
					Drop(m);
				}
			}
		}
		++g_win.frames;
	}

	void Reset()
	{
		for (auto& m : g_maps) {
			if (m.camera.load(std::memory_order_relaxed)) {
				Drop(m);
			}
		}
	}

	void Report()
	{
		if (!g_installed || g_win.frames == 0) {
			return;
		}
		auto&        w = g_win;
		const double f = static_cast<double>(w.frames);
		logger::info("[LightCache] {} | maps/frame: from cache {:.1f}, built {:.2f}, drawn normally {:.1f} | meshes/frame saved {:.0f}, still drawn (moving) {:.0f} | "
					 "rebuilds: light moved {}, mesh added {}, mesh removed {}, mesh started moving {} | failed {}",
			Config::lightShadowCache.enabled ? "ON" : "OFF", w.cached / f, w.builds / f, w.normal / f, g_saved.exchange(0) / f, g_drawnDynamic.exchange(0) / f, w.lightMoved,
			g_invAddedJobs.exchange(0), w.invRemoved, g_invPromotedJobs.exchange(0), w.buildFailed);
		w = {};
	}
}
