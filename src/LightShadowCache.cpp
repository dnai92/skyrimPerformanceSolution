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
			kWait,    // wie normal, zaehlt aber Bewegliche (Aufbau bevorzugt ohne Figuren im Radius)
			kBuild,   // nur Unbewegliche zeichnen, danach in den Cache kopieren
			kCached   // Cache zurueckkopieren, nur Bewegliche (und neu Hinzugekommene) zeichnen
		};

		constexpr std::uint32_t kMaxLights = 16;      // gleichzeitig verwaltete Schattenlichter
		constexpr std::uint32_t kMaxCams = 4;         // Schattenkarten je Licht (Fackel: 2 Halbkugeln)
		constexpr std::uint32_t kStableFrames = 3;    // so lange muss ein Licht ruhen, bevor aufgebaut wird
		// spaetestens dann aufbauen, auch mit Figuren im Radius (deren Schatten fehlen im Aufbau-Frame -> sichtbares
		// Blinken; 1.0.27 mit 30 Frames: rhythmisch ~1x pro Sekunde in der Drachenfeste)
		constexpr std::uint32_t kMaxWaitFrames = 600;
		constexpr std::uint32_t kMaxExtras = 256;     // so viele neu hinzugekommene werden zusaetzlich gezeichnet
		constexpr float         kMoveEpsilon = 0.5f;  // Einheiten - darueber gilt das Licht als bewegt (Flackern)

		// Eine Fackel zeichnet beide Halbkugeln in DIESELBE Ebene der Schattenkarten-Sammlung (zwei Bildhaelften).
		// Tiefen-Texturen lassen sich nur ganz kopieren -> Cache je Licht: vor der ersten Karte zurueckkopieren,
		// nach der letzten sichern. Lichter, deren Karten in verschiedenen Ebenen liegen, werden nicht gecacht.
		struct LightState
		{
			std::atomic<const RE::BSShadowLight*>                  light{ nullptr };
			std::array<std::atomic<const RE::NiCamera*>, kMaxCams> cams{};
			std::atomic<int>                                       mode{ static_cast<int>(Mode::kNormal) };
			// Main-Thread
			std::array<RE::NiTransform, kMaxCams> pose{};
			std::uint32_t                         camCount = 0;
			float                                 radius = 0;
			std::uint32_t                         stableFrames = 0;
			std::uint32_t                         waitFrames = 0;
			std::uint32_t                         lastSeen = 0;
			bool                                  valid = false;        // Cache-Inhalt passt
			bool                                  built = false;        // Aufbau-Frame: Kopie gesichert
			bool                                  drawn = false;        // in diesem Frame gezeichnet
			bool                                  unsupported = false;  // Karten in verschiedenen Ebenen
			REX::W32::ID3D11Texture2D*            tex = nullptr;        // eigene Kopie der Ebene
			REX::W32::ID3D11Texture2D*            boundTex = nullptr;   // Ebene der 1. Karte in diesem Frame (Vergleich)
			std::uint32_t                         boundSub = 0;
			std::uint32_t                         texW = 0, texH = 0, texFormat = 0;
			// Culling-Jobs (unter lock)
			struct Entry
			{
				RE::NiTransform pose;  // Lage beim Aufbau
				std::uint32_t   seen;  // Frame der letzten Pruefung (doppelte Aufnahme nur einmal zaehlen)
			};
			std::mutex                                       lock;
			std::unordered_map<const RE::BSGeometry*, Entry> statics;   // Inhalt des Caches
			std::unordered_set<const RE::BSGeometry*>        extras;    // seit dem Aufbau hinzugekommen: zusaetzlich zeichnen
			std::unordered_set<const RE::BSGeometry*>        promoted;  // gelernte Bewegliche
			std::uint32_t                                    matched = 0;
			std::uint32_t                                    dynamicSeen = 0;  // Bewegliche im letzten Frame
			bool                                             broken = false;   // bewegt/weggefallen -> neu aufbauen
		};
		std::array<LightState, kMaxLights> g_lights;
		std::atomic<std::uint32_t>         g_frame{ 0 };
		bool                               g_installed = false;

		struct Window
		{
			std::uint64_t frames = 0, cachedMaps = 0, builds = 0, normalMaps = 0;
			std::uint64_t lightMoved = 0, removed = 0, promoted = 0, extraFull = 0, buildFailed = 0, unsupported = 0;
		} g_win;
		std::atomic<std::uint64_t> g_saved{ 0 }, g_drawnDynamic{ 0 }, g_drawnExtra{ 0 };

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

		// Bewegt im Sinne des Schattens? Winziges Zittern (Physik in Ruhe, Rechenungenauigkeit) zaehlt nicht - die
		// gerundete Pruefsumme von 1.0.22-1.0.27 sprang dabei ueber Rundungsgrenzen und loeste staendig Neuaufbau aus
		bool MeshMoved(const RE::NiTransform& a_a, const RE::NiTransform& a_b) noexcept
		{
			const float dx = a_a.translate.x - a_b.translate.x, dy = a_a.translate.y - a_b.translate.y, dz = a_a.translate.z - a_b.translate.z;
			if (dx * dx + dy * dy + dz * dz > 2.0f * 2.0f || std::abs(a_a.scale - a_b.scale) > 0.01f) {
				return true;
			}
			for (int r = 0; r < 3; ++r) {
				for (int c = 0; c < 3; ++c) {
					if (std::abs(a_a.rotate.entry[r][c] - a_b.rotate.entry[r][c]) > 0.01f) {
						return true;
					}
				}
			}
			return false;
		}

		std::mutex                                     g_moverLock;
		std::unordered_map<std::string, std::uint32_t> g_movers;  // Namen der Meshes, die einen Neuaufbau ausloesten

		void RecordMover(const RE::BSGeometry& a_geom) noexcept
		{
			try {
				std::scoped_lock lock(g_moverLock);
				if (g_movers.size() < 256) {
					++g_movers[a_geom.name.c_str() ? a_geom.name.c_str() : ""];
				}
			} catch (...) {
			}
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

		LightState* FindByCamera(const RE::NiCamera* a_camera, std::uint32_t* a_index = nullptr) noexcept
		{
			for (auto& l : g_lights) {
				if (!l.light.load(std::memory_order_relaxed)) {
					continue;
				}
				for (std::uint32_t i = 0; i < kMaxCams; ++i) {
					if (l.cams[i].load(std::memory_order_relaxed) == a_camera) {
						if (a_index) {
							*a_index = i;
						}
						return &l;
					}
				}
			}
			return nullptr;
		}

		LightState* FindByLight(const RE::BSShadowLight* a_light) noexcept
		{
			for (auto& l : g_lights) {
				if (l.light.load(std::memory_order_relaxed) == a_light) {
					return &l;
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

		void ClearSets(LightState& a_l) noexcept
		{
			std::scoped_lock lock(a_l.lock);
			a_l.statics.clear();
			a_l.extras.clear();
			a_l.matched = 0;
			a_l.dynamicSeen = 0;
			a_l.broken = false;
		}

		void Drop(LightState& a_l) noexcept
		{
			if (a_l.tex) {
				a_l.tex->Release();
				a_l.tex = nullptr;
			}
			ClearSets(a_l);
			{
				std::scoped_lock lock(a_l.lock);
				a_l.promoted.clear();
			}
			a_l.valid = a_l.built = a_l.drawn = a_l.unsupported = false;
			a_l.stableFrames = a_l.waitFrames = a_l.camCount = 0;
			a_l.mode.store(static_cast<int>(Mode::kNormal), std::memory_order_relaxed);
			for (auto& c : a_l.cams) {
				c.store(nullptr, std::memory_order_relaxed);
			}
			a_l.light.store(nullptr, std::memory_order_relaxed);
		}

		// Ziel-Textur und Unterressource einer Schattenkarte
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

		bool EnsureCacheTexture(LightState& a_l, REX::W32::ID3D11Texture2D* a_src) noexcept
		{
			REX::W32::D3D11_TEXTURE2D_DESC desc{};
			a_src->GetDesc(&desc);
			if (a_l.tex && a_l.texW == desc.width && a_l.texH == desc.height && a_l.texFormat == static_cast<std::uint32_t>(desc.format)) {
				return true;
			}
			if (a_l.tex) {
				a_l.tex->Release();
				a_l.tex = nullptr;
			}
			desc.arraySize = 1;
			desc.mipLevels = 1;
			desc.usage = REX::W32::D3D11_USAGE_DEFAULT;
			desc.bindFlags = 0;
			desc.cpuAccessFlags = 0;
			desc.miscFlags = 0;
			const auto device = RE::BSGraphics::Renderer::GetDevice();
			if (!device || device->CreateTexture2D(&desc, nullptr, &a_l.tex) < 0 || !a_l.tex) {
				a_l.tex = nullptr;
				return false;
			}
			a_l.texW = desc.width;
			a_l.texH = desc.height;
			a_l.texFormat = static_cast<std::uint32_t>(desc.format);
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

		// Das aktive Schattenlicht zur Kamera samt Kartenindex (Main-Thread)
		const RE::BSShadowLight* FindActiveLight(const RE::NiCamera* a_camera, std::uint32_t& a_index) noexcept
		{
			const auto ssn = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
			if (!ssn) {
				return nullptr;
			}
			for (const auto& light : ssn->GetRuntimeData().activeShadowLights) {
				if (!light) {
					continue;
				}
				const auto& descs = light->GetRuntimeData().shadowmapDescriptors;
				for (std::uint32_t i = 0; i < descs.size(); ++i) {
					if (descs[i].camera.get() == a_camera) {
						a_index = i;
						return light.get();
					}
				}
			}
			return nullptr;
		}

		// ID 107604 (+0x167) ruft hier das Zeichnen einer Schattenkarte auf (Kamera, Shader-Akkumulator, Flags)
		// Gebundener Tiefenpuffer: Textur (nicht besessen, lebt in der Engine) und Unterressource der gebundenen Ebene
		REX::W32::ID3D11Texture2D* BoundDepth(std::uint32_t& a_sub) noexcept
		{
			const auto                        context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
			REX::W32::ID3D11RenderTargetView* rtvs[1]{};
			REX::W32::ID3D11DepthStencilView* dsv = nullptr;
			context->OMGetRenderTargets(1, rtvs, &dsv);
			if (rtvs[0]) {
				rtvs[0]->Release();
			}
			if (!dsv) {
				return nullptr;
			}
			REX::W32::D3D11_DEPTH_STENCIL_VIEW_DESC d{};
			dsv->GetDesc(&d);
			REX::W32::ID3D11Resource* res = nullptr;
			dsv->GetResource(&res);
			dsv->Release();
			if (!res) {
				return nullptr;
			}
			const auto tex = static_cast<REX::W32::ID3D11Texture2D*>(res);  // DSV einer 2D-Textur(-Sammlung)
			res->Release();  // die Engine haelt die Textur
			REX::W32::D3D11_TEXTURE2D_DESC td{};
			tex->GetDesc(&td);
			const auto dim = static_cast<int>(d.viewDimension);
			if (td.sampleDesc.count != 1) {
				return nullptr;
			}
			if (dim == 4) {  // TEXTURE2DARRAY
				if (d.texture2DArray.arraySize != 1 || d.texture2DArray.firstArraySlice >= td.arraySize) {
					return nullptr;
				}
				a_sub = d.texture2DArray.firstArraySlice * td.mipLevels + d.texture2DArray.mipSlice;
			} else if (dim == 3) {  // TEXTURE2D
				a_sub = d.texture2D.mipSlice;
			} else {
				return nullptr;
			}
			return tex;
		}

		// Diagnose (Analyse-Protokoll): wohin zeichnet jede Schattenkarte wirklich? Ebene/Ausschnitt laut Deskriptor
		// gegen den tatsaechlich gebundenen Tiefenpuffer und Viewport (vor und nach dem Zeichnen)
		std::uint32_t g_targetDiagLeft = 0;
		std::uint32_t g_targetDiagFrame = 0;

		struct BoundTarget
		{
			std::int64_t  slice = -1, arraySize = -1;
			float         vx = -1, vy = -1, vw = -1, vh = -1;
		};

		BoundTarget ReadBound() noexcept
		{
			BoundTarget b;
			const auto  context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
			REX::W32::ID3D11RenderTargetView* rtvs[1]{};
			REX::W32::ID3D11DepthStencilView* dsv = nullptr;
			context->OMGetRenderTargets(1, rtvs, &dsv);
			if (rtvs[0]) {
				rtvs[0]->Release();
			}
			if (dsv) {
				REX::W32::D3D11_DEPTH_STENCIL_VIEW_DESC d{};
				dsv->GetDesc(&d);
				if (static_cast<int>(d.viewDimension) == 4) {  // TEXTURE2DARRAY
					b.slice = d.texture2DArray.firstArraySlice;
					b.arraySize = d.texture2DArray.arraySize;
				} else {
					b.slice = 1000 + static_cast<int>(d.viewDimension);
				}
				dsv->Release();
			}
			std::uint32_t            n = 1;
			REX::W32::D3D11_VIEWPORT vp{};
			context->RSGetViewports(&n, &vp);
			if (n) {
				b.vx = vp.topLeftX;
				b.vy = vp.topLeftY;
				b.vw = vp.width;
				b.vh = vp.height;
			}
			return b;
		}

		struct DrawShadowmap
		{
			static void DiagDraw(RE::NiCamera* a_camera, void* a_accumulator, std::uint32_t a_flags)
			{
				std::uint32_t idx = 0;
				const auto    light = FindActiveLight(a_camera, idx);
				const auto    before = ReadBound();
				func(a_camera, a_accumulator, a_flags);
				const auto after = ReadBound();
				if (!light) {
					logger::info("[LightCache-Target] sun/other camera {} | bound slice {} (of {}) vp {:.0f},{:.0f} {:.0f}x{:.0f} -> after slice {} vp {:.0f},{:.0f} {:.0f}x{:.0f}",
						static_cast<const void*>(a_camera), before.slice, before.arraySize, before.vx, before.vy, before.vw, before.vh, after.slice, after.vx, after.vy, after.vw, after.vh);
					return;
				}
				const auto& d = light->GetRuntimeData().shadowmapDescriptors[idx];
				const auto  port = reinterpret_cast<const std::int32_t*>(&d.port);  // NiRect-Felder sind protected
				logger::info("[LightCache-Target] light {} ({}) map {}/{} | desc target {} slice {} port L{} R{} T{} B{} clear {} | bound slice {} (of {}) vp {:.0f},{:.0f} {:.0f}x{:.0f} -> after slice {} vp {:.0f},{:.0f} {:.0f}x{:.0f}",
					static_cast<const void*>(light), const_cast<RE::BSShadowLight*>(light)->GetIsParabolicLight() ? "omni" : (const_cast<RE::BSShadowLight*>(light)->GetIsFrustumLight() ? "spot" : "other"), idx,
					light->GetRuntimeData().shadowmapDescriptors.size(), static_cast<std::uint32_t>(d.renderTarget), d.shadowmapIndex, port[0], port[1], port[2],
					port[3], d.clearRenderTarget, before.slice, before.arraySize, before.vx, before.vy, before.vw, before.vh, after.slice, after.vx, after.vy, after.vw, after.vh);
			}

			static void thunk(RE::NiCamera* a_camera, void* a_accumulator, std::uint32_t a_flags)
			{
				// Diagnose: alle 5 s die Schattenkarten von 3 Frames protokollieren
				if (g_targetDiagLeft > 0 && Config::analysis.load(std::memory_order_relaxed)) {
					DiagDraw(a_camera, a_accumulator, a_flags);
					return;
				}
				LightState* l = a_camera ? FindByCamera(a_camera) : nullptr;
				const auto  mode = l ? static_cast<Mode>(l->mode.load(std::memory_order_relaxed)) : Mode::kNormal;
				if (mode != Mode::kBuild && mode != Mode::kCached) {
					func(a_camera, a_accumulator, a_flags);
					return;
				}
				std::uint32_t idx = 0;
				const auto    light = FindActiveLight(a_camera, idx);
				if (!light || light != l->light.load(std::memory_order_relaxed)) {
					func(a_camera, a_accumulator, a_flags);
					l->valid = false;
					++g_win.buildFailed;
					return;
				}
				const auto& descs = light->GetRuntimeData().shadowmapDescriptors;
				// Ziel = der gerade gebundene Tiefenpuffer (nicht der Deskriptor: bei Fackeln steht in der 2. Halbkugel eine
				// veraltete Ebene, gezeichnet wird in die untere Haelfte der Ebene der 1. Halbkugel - Messung 1.0.26)
				std::uint32_t sub = 0;
				const auto    target = BoundDepth(sub);
				if (idx == 0) {
					l->boundTex = target;
					l->boundSub = sub;
				} else if (target != l->boundTex || sub != l->boundSub) {
					l->unsupported = true;  // Karten eines Lichts in verschiedenen Ebenen
				}
				if (!target || l->unsupported || !EnsureCacheTexture(*l, target)) {
					func(a_camera, a_accumulator, a_flags);
					l->valid = false;
					++g_win.buildFailed;
					return;
				}
				l->drawn = true;
				if (mode == Mode::kCached) {
					if (idx == 0) {
						CopyUnbound(target, sub, l->tex, 0);  // unbewegliche Schatten aller Karten zurueck (nach dem Leeren)
					}
					func(a_camera, a_accumulator, a_flags);  // nur Bewegliche
				} else {
					func(a_camera, a_accumulator, a_flags);  // nur Unbewegliche
					if (idx + 1 == descs.size()) {
						CopyUnbound(l->tex, 0, target, sub);  // nach der letzten Karte sichern
						l->built = true;
					}
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
		LightState* l = FindByCamera(a_camera);
		if (!l) {
			return false;
		}
		const auto mode = static_cast<Mode>(l->mode.load(std::memory_order_relaxed));
		if (mode == Mode::kNormal) {
			return false;
		}
		bool dynamic = IsDynamic(a_geom);
		try {
			std::scoped_lock lock(l->lock);
			dynamic = dynamic || l->promoted.contains(&a_geom);
			if (mode == Mode::kWait) {
				if (dynamic) {
					++l->dynamicSeen;
				}
				return false;
			}
			if (mode == Mode::kBuild) {
				if (dynamic) {
					return true;  // Aufbau-Frame: nur Unbewegliche in die Karte
				}
				l->statics[&a_geom] = { a_geom.world, 0 };
				return false;
			}
			// Cache-Frame
			if (dynamic) {
				g_drawnDynamic.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			const auto it = l->statics.find(&a_geom);
			if (it == l->statics.end()) {
				// neu im Lichtradius: zusaetzlich zeichnen, der Cache bleibt gueltig (zu viele -> neu aufbauen)
				l->extras.insert(&a_geom);
				g_drawnExtra.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			const auto frame = g_frame.load(std::memory_order_relaxed);
			if (it->second.seen != frame) {
				it->second.seen = frame;
				++l->matched;
			}
			if (MeshMoved(it->second.pose, a_geom.world)) {
				l->promoted.insert(&a_geom);  // bewegt sich -> ab jetzt immer neu zeichnen; alter Schatten steckt im Cache
				RecordMover(a_geom);
				l->broken = true;
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
		const auto frame = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
		const bool enabled = Config::lightShadowCache.enabled && Config::masterEnabled.load(std::memory_order_relaxed);

		// 1. letzten Frame auswerten
		for (auto& l : g_lights) {
			if (!l.light.load(std::memory_order_relaxed)) {
				continue;
			}
			const auto mode = static_cast<Mode>(l.mode.load(std::memory_order_relaxed));
			if (l.unsupported) {
				++g_win.unsupported;
			}
			if ((mode == Mode::kBuild || mode == Mode::kCached) && l.drawn) {
				std::scoped_lock lock(l.lock);
				if (mode == Mode::kBuild) {
					l.valid = l.built && !l.statics.empty() && !l.unsupported;
					++g_win.builds;
				} else {
					if (l.broken) {
						++g_win.promoted;
						l.valid = false;
					} else if (l.matched != l.statics.size()) {
						++g_win.removed;  // ein Unbewegliches fehlt (weggenommen, Tuer geoeffnet ...) -> Geisterschatten
						l.valid = false;
					} else if (l.extras.size() > kMaxExtras) {
						++g_win.extraFull;
						l.valid = false;
					}
				}
				l.broken = false;
				l.matched = 0;
			}
			l.drawn = false;
			l.built = false;
		}

		// 2. Modus je Licht fuer diesen Frame
		const auto ssn = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		if (ssn) {
			auto& data = ssn->GetRuntimeData();
			for (const auto& lightPtr : data.activeShadowLights) {
				const auto light = lightPtr.get();
				if (!light || light == data.sunShadowDirLight) {
					continue;
				}
				const auto& descs = light->GetRuntimeData().shadowmapDescriptors;
				if (descs.empty() || descs.size() > kMaxCams) {
					continue;
				}
				LightState* l = FindByLight(light);
				if (!l) {
					for (auto& slot : g_lights) {
						if (!slot.light.load(std::memory_order_relaxed)) {
							slot.camCount = 0;
							slot.stableFrames = slot.waitFrames = 0;
							slot.valid = slot.unsupported = false;
							slot.light.store(light, std::memory_order_relaxed);
							l = &slot;
							break;
						}
					}
					if (!l) {
						continue;  // voll: dieses Licht normal zeichnen
					}
				}
				l->lastSeen = frame;
				const auto  niLight = light->light.get();
				const float radius = niLight ? niLight->GetLightRuntimeData().radius.x : 0.0f;
				bool        moved = descs.size() != l->camCount || std::abs(radius - l->radius) > kMoveEpsilon;
				for (std::uint32_t i = 0; i < descs.size(); ++i) {
					const auto cam = descs[i].camera.get();
					if (!cam) {
						moved = true;
						continue;
					}
					if (l->cams[i].load(std::memory_order_relaxed) != cam || PoseChanged(cam->world, l->pose[i])) {
						moved = true;
					}
					l->pose[i] = cam->world;
					l->cams[i].store(cam, std::memory_order_relaxed);
				}
				for (std::uint32_t i = static_cast<std::uint32_t>(descs.size()); i < kMaxCams; ++i) {
					l->cams[i].store(nullptr, std::memory_order_relaxed);
				}
				l->camCount = static_cast<std::uint32_t>(descs.size());
				l->radius = radius;
				if (moved) {
					if (l->valid) {
						++g_win.lightMoved;
					}
					l->stableFrames = 0;
					l->waitFrames = 0;
					l->valid = false;
				} else {
					++l->stableFrames;
				}
				Mode mode = Mode::kNormal;
				if (enabled && !l->unsupported) {
					if (l->valid) {
						mode = Mode::kCached;
					} else if (l->stableFrames >= kStableFrames) {
						// Aufbau bevorzugt, wenn im letzten Frame keine Figur im Radius war (deren Schatten fehlen im
						// Aufbau-Frame); spaetestens nach kMaxWaitFrames trotzdem
						std::uint32_t dynamicSeen = 0;
						{
							std::scoped_lock lock(l->lock);
							dynamicSeen = std::exchange(l->dynamicSeen, 0u);
						}
						const auto prev = static_cast<Mode>(l->mode.load(std::memory_order_relaxed));
						if ((prev == Mode::kWait && dynamicSeen == 0) || ++l->waitFrames > kMaxWaitFrames) {
							mode = Mode::kBuild;
							l->waitFrames = 0;
							ClearSets(*l);
						} else {
							mode = Mode::kWait;
						}
					}
				}
				if (mode != Mode::kCached) {
					g_win.normalMaps += descs.size();
				} else {
					g_win.cachedMaps += descs.size();
				}
				l->mode.store(static_cast<int>(mode), std::memory_order_relaxed);
			}
		}
		// Lichter, die gerade nicht aktiv sind: normal; lange nicht gesehen oder Cache aus -> freigeben
		for (auto& l : g_lights) {
			if (l.light.load(std::memory_order_relaxed) && l.lastSeen != frame) {
				l.mode.store(static_cast<int>(Mode::kNormal), std::memory_order_relaxed);
				if (frame - l.lastSeen > 600 || !enabled) {
					Drop(l);
				}
			} else if (l.light.load(std::memory_order_relaxed) && !enabled) {
				Drop(l);
			}
		}
		++g_win.frames;
		// Ziel-Diagnose: alle ~5 s zwei Frames lang jede Schattenkarte protokollieren
		if (g_targetDiagLeft > 0) {
			--g_targetDiagLeft;
		} else if (Config::analysis.load(std::memory_order_relaxed) && !enabled && ++g_targetDiagFrame % 300 == 0) {  // nur ohne Cache (umgeht ihn)
			g_targetDiagLeft = 2;
		}
	}

	void Reset()
	{
		for (auto& l : g_lights) {
			if (l.light.load(std::memory_order_relaxed)) {
				Drop(l);
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
		logger::info("[LightCache] {} | maps/frame from cache {:.1f}, drawn normally {:.1f} | builds/frame {:.3f} | meshes/frame saved {:.0f}, still drawn: moving {:.0f}, added {:.0f} | "
					 "rebuilds: light moved {}, mesh removed {}, mesh started moving {}, too many added {} | failed {}, lights not supported {:.1f}",
			Config::lightShadowCache.enabled ? "ON" : "OFF", w.cachedMaps / f, w.normalMaps / f, w.builds / f, g_saved.exchange(0) / f, g_drawnDynamic.exchange(0) / f,
			g_drawnExtra.exchange(0) / f, w.lightMoved, w.removed, w.promoted, w.extraFull, w.buildFailed, w.unsupported / f);
		std::scoped_lock lock(g_moverLock);
		for (const auto& [name, count] : g_movers) {
			logger::info("[LightCache]   started moving: {}x {}", count, name);
		}
		g_movers.clear();
		w = {};
	}
}
