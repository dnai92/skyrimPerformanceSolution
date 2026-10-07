#include "InstancingAnalysis.h"
#include "Features.h"

#include "Config.h"
#include "DetourHelper.h"
#include "InstancedDraw.h"
#include "ShadowCulling.h"
#include "Stats.h"

#include <mutex>
#include <unordered_map>

namespace InstancingAnalysis
{
	namespace
	{
		constexpr std::uint32_t kReportFrames = 600;

		// Wird waehrend BSShadowDirectionalLight::Render gesetzt (Render-Thread = Main-Thread)
		std::atomic<bool> g_inSunShadows{ false };
		std::int64_t      g_sunRenderNs = 0;  // Summe im Report-Fenster (Main-Thread)

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

		// ---- Instancing-Analyse v2 (Schritt 1): realistische Gruppen fuer Instancing ----
		// Instanzen muessen im selben RenderBatches-Aufruf liegen (gleiche Kaskade/Renderziel, gleiche Shader-Technik)
		// und dieselbe Geometrie, dasselbe Vertex-Format und - bei Alpha-Test - dieselbe Textur haben.
		enum class Cat : std::uint32_t
		{
			kSimple,     // undurchsichtig, statisch -> Prototyp (Schritt 2)
			kAlphaTest,  // Alpha-Test (Blaetter, Zaeune mit Luecken) -> Schritt 3
			kTreeAnim,   // Baeume mit Windanimation (Sonderfall im Shader)
			kLod,        // LOD-Landschaft/-Objekte
			kSkinned,    // Charaktere/Kreaturen
			kOther,      // kein TriShape / ohne Buffer
			kTotal
		};
		constexpr std::array<const char*, static_cast<std::size_t>(Cat::kTotal)> kCatNames{ "simple", "alpha test", "tree animation", "LOD", "skinned", "other" };

		struct Key2
		{
			std::uint32_t batch;  // laufende Nummer des RenderBatches-Aufrufs im Frame
			const void*   vertexBuffer;
			const void*   indexBuffer;
			std::uint64_t vertexDesc;
			std::uint32_t technique;
			const void*   texture;  // nur bei Alpha-Test
			std::uint32_t cat;

			bool operator==(const Key2&) const = default;
		};

		struct Key2Hash
		{
			std::size_t operator()(const Key2& k) const noexcept
			{
				std::size_t h = k.batch;
				const auto  mix = [&](std::size_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
				mix(std::hash<const void*>{}(k.vertexBuffer));
				mix(std::hash<const void*>{}(k.indexBuffer));
				mix(std::hash<std::uint64_t>{}(k.vertexDesc));
				mix(k.technique);
				mix(std::hash<const void*>{}(k.texture));
				mix(k.cat);
				return h;
			}
		};

		std::unordered_map<Key2, std::uint32_t, Key2Hash> g_frameGroups2;
		std::uint32_t                                     g_batchCounter = 0;  // nur waehrend Sonnenschatten gezaehlt
		std::array<std::uint32_t, static_cast<std::size_t>(Cat::kTotal)> g_frameCat{};

		struct Window2
		{
			std::uint64_t frames = 0;
			std::uint64_t batches = 0;
			std::array<std::uint64_t, static_cast<std::size_t>(Cat::kTotal)> draws{};
			std::array<std::uint64_t, static_cast<std::size_t>(Cat::kTotal)> saved{};      // eingesparte Draws bei Instancing ab 2
			std::array<std::uint64_t, static_cast<std::size_t>(Cat::kTotal)> savedMin4{};  // nur Gruppen ab 4 Instanzen
			std::array<std::uint64_t, static_cast<std::size_t>(Cat::kTotal)> groups{};
			double        renderMs = 0.0;
		} g_window2;

		// Techniken der einfachen Kategorie: Draws und moegliche Ersparnis (fuer die Freischaltung in sTechnique)
		struct TechStat
		{
			std::uint64_t draws = 0, saved = 0, twoSidedDraws = 0;
		};
		std::unordered_map<std::uint32_t, TechStat> g_techStats;

		Cat Classify(RE::BSGeometry& a_geom, const void*& a_texture) noexcept;
		bool PositionIsFloat32(std::uint64_t a_desc) noexcept;

		// ---- Shadow-Instancing (Schritt 2) ----
		// Innerhalb eines RenderBatches-Aufrufs der Sonnenschatten: das erste Mesh einer Gruppe zeichnet die Engine,
		// alle weiteren gleichen Meshes werden gesammelt und am Ende des Batches mit einem Draw Call nachgezeichnet.
		struct InstKey
		{
			const void*   vertexBuffer;
			const void*   indexBuffer;
			std::uint64_t vertexDesc;
			std::uint32_t technique;
			bool          twoSided;
			bool operator==(const InstKey&) const = default;
		};
		struct InstKeyHash
		{
			std::size_t operator()(const InstKey& k) const noexcept
			{
				std::size_t h = std::hash<const void*>{}(k.vertexBuffer);
				h ^= std::hash<const void*>{}(k.indexBuffer) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
				h ^= std::hash<std::uint64_t>{}(k.vertexDesc) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
				h ^= (static_cast<std::size_t>(k.technique) << 1 | (k.twoSided ? 1 : 0)) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
				return h;
			}
		};
		struct InstGroup
		{
			InstancedDraw::Group                 group{};
			std::vector<InstancedDraw::Instance> instances;
		};

		bool                                                    g_instBatchActive = false;  // aktueller RenderBatches-Aufruf sammelt
		bool                                                    g_instCaptured = false;
		bool                                                    g_instInitFailed = false;
		std::unordered_map<InstKey, std::uint32_t, InstKeyHash> g_instIndex;
		std::vector<InstGroup>                                  g_instGroups;
		std::uint32_t                                           g_instUsed = 0;  // benutzte Eintraege in g_instGroups
		std::vector<InstancedDraw::Instance>                    g_instFlat;
		std::vector<InstancedDraw::Group>                       g_instFlatGroups;

		void* D3DContext() noexcept
		{
			const auto r = RE::BSGraphics::Renderer::GetSingleton();
			return r ? static_cast<void*>(r->GetRuntimeData().context) : nullptr;
		}

		bool InstancingWanted() noexcept
		{
			const auto& cfg = Config::shadowInstancing;
			// SE 1.5.97: Shader-/Konstantenpuffer-Interna nicht geprueft -> nie
			if (!cfg.enabled || !Config::masterEnabled.load(std::memory_order_relaxed) || g_instInitFailed || REL::Module::IsSE()) {
				return false;
			}
			if (!InstancedDraw::Ready()) {
				char err[512]{};
				if (!InstancedDraw::Init(RE::BSGraphics::Renderer::GetDevice(), err, sizeof(err))) {
					g_instInitFailed = true;
					logger::error("ShadowInstancing: initialization failed - {}", err);
					return false;
				}
				logger::info("ShadowInstancing: shaders and buffers created");
			}
			return true;
		}

		// Qualifiziert sich der Pass fuer Instancing? (einfach: statisch, undurchsichtig, einseitig, gewuenschte Technik)
		bool Qualifies(const RE::BSRenderPass& a_pass, bool a_alphaTest) noexcept
		{
			const auto& cfg = Config::shadowInstancing;
			if (a_alphaTest || !a_pass.geometry) {
				return false;
			}
			bool allowed = false;
			for (std::uint32_t i = 0; i < cfg.techniqueCount; ++i) {
				allowed = allowed || a_pass.passEnum == cfg.techniques[i];
			}
			if (!allowed) {
				return false;
			}
			auto& geom = *a_pass.geometry;
			const void* texture = nullptr;
			if (Classify(geom, texture) != Cat::kSimple) {
				return false;
			}
			const auto prop = geom.GetGeometryRuntimeData().shaderProperty.get();
			if (!cfg.allowTwoSided && prop && prop->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided)) {
				return false;
			}
			return geom.AsTriShape() != nullptr;
		}

		InstancedDraw::Instance MakeInstance(const RE::BSGeometry& a_geom) noexcept
		{
			const auto& w = a_geom.world;
			RE::NiPoint3 adjust{};
			if (const auto state = RE::BSGraphics::RendererShadowState::GetSingleton()) {
				adjust = state->GetRuntimeData().posAdjust.getEye();
			}
			InstancedDraw::Instance inst{};
			for (int r = 0; r < 3; ++r) {
				for (int c = 0; c < 3; ++c) {
					inst.rows[r][c] = w.rotate.entry[r][c] * w.scale;
				}
			}
			inst.rows[0][3] = w.translate.x - adjust.x;
			inst.rows[1][3] = w.translate.y - adjust.y;
			inst.rows[2][3] = w.translate.z - adjust.z + Config::shadowInstancing.debugOffsetZ;
			return inst;
		}

		// Diagnose: World-Matrix der Engine (PerGeometry b2, c1..c4) und CameraViewProj (b12, c8..c11) mit unserer vergleichen
		std::uint32_t g_debugCompareBudget = 3;  // Anzahl Vergleiche pro Report-Fenster
		void DebugCompareWorld(const RE::BSRenderPass& a_pass) noexcept
		{
			if (g_debugCompareBudget == 0 || !a_pass.geometry) {
				return;
			}
			--g_debugCompareBudget;
			float cb2[20]{};
			float cb12[48]{};
			const bool ok2 = InstancedDraw::DebugReadVSConstants(D3DContext(), 2, cb2, 20);
			const bool ok12 = InstancedDraw::DebugReadVSConstants(D3DContext(), 12, cb12, 48);
			const auto ours = MakeInstance(*a_pass.geometry);
			char       state[512]{};
			InstancedDraw::DebugDescribeState(D3DContext(), state, sizeof(state));
			if (const auto rd = a_pass.geometry->GetGeometryRuntimeData().rendererData) {
				auto desc = rd->vertexDesc;
				const auto tri = a_pass.geometry->AsTriShape();
				logger::info("[Instancing-Debug]   engine state after draw: {} || our values: VB {} stride {} IB {} triangles {} FullPrec {} Desc {:016X}", state,
					static_cast<const void*>(rd->vertexBuffer), desc.GetSize(), static_cast<const void*>(rd->indexBuffer), tri ? tri->GetTrishapeRuntimeData().triangleCount : 0,
					PositionIsFloat32(std::bit_cast<std::uint64_t>(rd->vertexDesc)), std::bit_cast<std::uint64_t>(rd->vertexDesc));
			}
			const auto& w = a_pass.geometry->world;
			logger::info("[Instancing-Debug] '{}' world pos ({:.1f} {:.1f} {:.1f}) scale {:.3f} | CB2 read {} | CB12 read {}", a_pass.geometry->name.c_str(),
				w.translate.x, w.translate.y, w.translate.z, w.scale, ok2, ok12);
			for (int r = 0; r < 4; ++r) {
				logger::info("[Instancing-Debug]   engine world row {}: {:10.4f} {:10.4f} {:10.4f} {:10.4f}   | ours: {}", r, cb2[4 + r * 4], cb2[5 + r * 4], cb2[6 + r * 4], cb2[7 + r * 4],
					r < 3 ? std::format("{:10.4f} {:10.4f} {:10.4f} {:10.4f}", ours.rows[r][0], ours.rows[r][1], ours.rows[r][2], ours.rows[r][3]) : std::string("0 0 0 1"));
			}
			for (int r = 0; r < 4; ++r) {
				logger::info("[Instancing-Debug]   CB12 CameraViewProj row {}: {:10.5f} {:10.5f} {:10.5f} {:10.5f}", r, cb12[32 + r * 4], cb12[33 + r * 4], cb12[34 + r * 4], cb12[35 + r * 4]);
			}
		}

		// Positionsformat aus den Attribut-Offsets der Vertex-Beschreibung ableiten: Nibble 0 = Vertexgroesse/4,
		// Nibble 1..9 = Offset/4 der Attribute (Position, UV, UV2, Normal, Binormal, Farbe, Skin, Land, Augen).
		// Liegt das naechste Attribut 16 Byte hinter der Position, ist sie float32x4, bei 8 Byte float16x4.
		// (Das Flag VF_FULLPREC ist dafuer nicht verlaesslich: im Test 0.11.x war es aus, obwohl float32 vorlag.)
		bool PositionIsFloat32(std::uint64_t a_desc) noexcept
		{
			const std::uint32_t size = static_cast<std::uint32_t>(a_desc & 0xF) * 4;
			const std::uint32_t posOffset = static_cast<std::uint32_t>((a_desc >> 4) & 0xF) * 4;
			std::uint32_t       next = size;
			for (int i = 2; i <= 9; ++i) {
				const std::uint32_t off = static_cast<std::uint32_t>((a_desc >> (4 * i)) & 0xF) * 4;
				if (off > posOffset && off < next) {
					next = off;
				}
			}
			return next - posOffset >= 16;
		}

		// true = Pass wurde uebernommen (Engine soll ihn NICHT zeichnen)
		bool CollectInstance(RE::BSRenderPass& a_pass) noexcept
		{
			auto&       geom = *a_pass.geometry;
			const auto  rd = geom.GetGeometryRuntimeData().rendererData;
			const auto  tri = geom.AsTriShape();
			const auto    prop = geom.GetGeometryRuntimeData().shaderProperty.get();
			const bool    twoSided = prop && prop->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided);
			const InstKey key{ rd->vertexBuffer, rd->indexBuffer, std::bit_cast<std::uint64_t>(rd->vertexDesc), a_pass.passEnum, twoSided };
			try {
				const auto [it, inserted] = g_instIndex.try_emplace(key, g_instUsed);
				if (inserted) {
					if (g_instUsed == g_instGroups.size()) {
						g_instGroups.emplace_back();
					}
					auto& g = g_instGroups[g_instUsed++];
					auto  desc = rd->vertexDesc;
					g.group = InstancedDraw::Group{ rd->vertexBuffer, rd->indexBuffer, desc.GetSize(), static_cast<std::uint32_t>(tri->GetTrishapeRuntimeData().triangleCount) * 3u,
						PositionIsFloat32(std::bit_cast<std::uint64_t>(rd->vertexDesc)), 0, 0, (a_pass.passEnum & 0x8000) != 0, twoSided };
					g.instances.clear();
					return false;  // erstes Mesh zeichnet die Engine
				}
				g_instGroups[it->second].instances.push_back(MakeInstance(geom));
				return !Config::shadowInstancing.verify;
			} catch (...) {
				return false;
			}
		}

		void FlushInstances() noexcept
		{
			const auto& cfg = Config::shadowInstancing;
			g_instFlat.clear();
			g_instFlatGroups.clear();
			for (std::uint32_t i = 0; i < g_instUsed; ++i) {
				auto& g = g_instGroups[i];
				// Gruppen unter der Mindestgroesse: fehlende Meshes trotzdem per Instancing nachzeichnen (sonst fehlen sie)
				if (g.instances.empty()) {
					continue;
				}
				g.group.firstInstance = static_cast<std::uint32_t>(g_instFlat.size());
				g.group.instanceCount = static_cast<std::uint32_t>(g.instances.size());
				g_instFlat.insert(g_instFlat.end(), g.instances.begin(), g.instances.end());
				g_instFlatGroups.push_back(g.group);
			}
			if (!g_instFlatGroups.empty() && cfg.debugMode != 2 && cfg.debugMode != 3) {
				if (InstancedDraw::Flush(D3DContext(), g_instFlat.data(), static_cast<std::uint32_t>(g_instFlat.size()), g_instFlatGroups.data(),
						static_cast<std::uint32_t>(g_instFlatGroups.size()), (cfg.technique & 0x8000) != 0, cfg.debugMode == 1)) {
					for (std::size_t n = 0; n < g_instFlat.size(); ++n) {
						Stats::Count(Stats::Counter::InstancedMeshes);
					}
					for (std::size_t n = 0; n < g_instFlatGroups.size(); ++n) {
						Stats::Count(Stats::Counter::InstancedCalls);
					}
				}
			}
			g_instIndex.clear();
			g_instUsed = 0;
			InstancedDraw::ReleaseState();
			g_instCaptured = false;
		}

		// BSBatchRenderer::RenderBatches (dieselbe Funktion wie in Community Shaders FrameAnnotations): Batch-Grenzen
		struct RenderBatches
		{
			static bool thunk(void* a_renderer, std::uint32_t* a_currentPass, std::uint32_t* a_bucketIndex, void* a_passIndexList, std::uint32_t a_renderFlags)
			{
				const bool sun = g_inSunShadows.load(std::memory_order_relaxed);
				if (sun) {
					++g_batchCounter;
				}
				g_instBatchActive = sun && !g_instBatchActive && InstancingWanted();
				const bool mine = g_instBatchActive;
				const bool r = func(a_renderer, a_currentPass, a_bucketIndex, a_passIndexList, a_renderFlags);
				if (mine) {
					FlushInstances();
					g_instBatchActive = false;
				}
				return r;
			}
			static inline bool (*func)(void*, std::uint32_t*, std::uint32_t*, void*, std::uint32_t) = nullptr;
		};

		Cat Classify(RE::BSGeometry& a_geom, const void*& a_texture) noexcept
		{
			a_texture = nullptr;
			auto& data = a_geom.GetGeometryRuntimeData();
			if (data.skinInstance) {
				return Cat::kSkinned;
			}
			if (a_geom.GetType().get() != RE::BSGeometry::Type::kTriShape || !data.rendererData || !data.rendererData->vertexBuffer || !data.rendererData->indexBuffer) {
				return Cat::kOther;
			}
			const auto prop = data.shaderProperty.get();
			if (prop) {
				using F = RE::BSShaderProperty::EShaderPropertyFlag;
				if (prop->flags.any(F::kLODLandscape, F::kLODObjects, F::kHDLODObjects)) {
					return Cat::kLod;
				}
				if (prop->flags.any(F::kTreeAnim)) {
					return Cat::kTreeAnim;
				}
			}
			const auto alpha = data.alphaProperty.get();
			if (alpha && alpha->GetAlphaTesting()) {
				if (prop) {
					if (const auto lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(prop)) {
						if (const auto mat = static_cast<RE::BSLightingShaderMaterialBase*>(lighting->material)) {
							a_texture = mat->diffuseTexture.get();
						}
					}
				}
				return Cat::kAlphaTest;
			}
			return Cat::kSimple;
		}

		void Record2(RE::BSRenderPass& a_pass) noexcept
		{
			auto&       geom = *a_pass.geometry;
			const void* texture = nullptr;
			const auto  cat = Classify(geom, texture);
			const auto  ci = static_cast<std::size_t>(cat);
			std::scoped_lock lock{ g_lock };
			++g_frameCat[ci];
			if (cat == Cat::kSkinned || cat == Cat::kOther) {
				return;
			}
			const auto rd = geom.GetGeometryRuntimeData().rendererData;
			try {
				++g_frameGroups2[Key2{ g_batchCounter, rd->vertexBuffer, rd->indexBuffer, std::bit_cast<std::uint64_t>(rd->vertexDesc), a_pass.passEnum, texture, static_cast<std::uint32_t>(cat) }];
			} catch (...) {
			}
		}

		void EndFrame2()
		{
			std::scoped_lock lock{ g_lock };
			std::uint32_t    total = 0;
			for (auto c : g_frameCat) {
				total += c;
			}
			if (total == 0) {
				g_frameGroups2.clear();
				g_batchCounter = 0;
				return;
			}
			auto& w = g_window2;
			++w.frames;
			w.batches += g_batchCounter;
			for (std::size_t i = 0; i < g_frameCat.size(); ++i) {
				w.draws[i] += g_frameCat[i];
			}
			for (const auto& [key, count] : g_frameGroups2) {
				if (key.cat == static_cast<std::uint32_t>(Cat::kSimple)) {
					auto& ts = g_techStats[key.technique];
					ts.draws += count;
					ts.saved += count >= 2 ? count - 1 : 0;
				}
				if (count >= 2) {
					w.saved[key.cat] += count - 1;
					++w.groups[key.cat];
				}
				if (count >= 4) {
					w.savedMin4[key.cat] += count - 1;
				}
			}
			g_frameGroups2.clear();
			g_frameCat = {};
			g_batchCounter = 0;
		}

		void Report2(double a_sunRenderMs)
		{
			std::scoped_lock lock{ g_lock };
			auto&            w = g_window2;
			if (w.frames == 0) {
				return;
			}
			const double f = static_cast<double>(w.frames);
			std::uint64_t drawsAll = 0, savedAll = 0;
			for (std::size_t i = 0; i < w.draws.size(); ++i) {
				drawsAll += w.draws[i];
				savedAll += w.saved[i];
			}
			logger::info("[Instancing-v2] {} frames | sun shadow draws/frame {:.0f} | RenderBatches calls/frame {:.1f} | sun render {:.2f} ms/frame (~{:.2f} us/draw)",
				w.frames, drawsAll / f, w.batches / f, a_sunRenderMs, drawsAll ? a_sunRenderMs * 1000.0 / (drawsAll / f) : 0.0);
			for (std::size_t i = 0; i < w.draws.size(); ++i) {
				logger::info("[Instancing-v2]   {:<15} {:6.0f} draws/frame | with instancing from 2: -{:5.0f} (groups {:4.0f}) | from 4: -{:5.0f}",
					kCatNames[i], w.draws[i] / f, w.saved[i] / f, w.groups[i] / f, w.savedMin4[i] / f);
			}
			const double simple = w.saved[0] / f;
			const double simpleAlpha = (w.saved[0] + w.saved[1]) / f;
			const double usPerDraw = drawsAll ? a_sunRenderMs * 1000.0 / (drawsAll / f) : 0.0;
			logger::info("[Instancing-v2]   => step 2 (simple): -{:.0f} draws/frame (~{:.2f} ms) | step 3 (+alpha test): -{:.0f} (~{:.2f} ms) | everything possible: -{:.0f}",
				simple, simple * usPerDraw / 1000.0, simpleAlpha, simpleAlpha * usPerDraw / 1000.0, savedAll / f);
			std::vector<std::pair<std::uint32_t, TechStat>> techs(g_techStats.begin(), g_techStats.end());
			std::ranges::sort(techs, [](const auto& a, const auto& b) { return a.second.saved > b.second.saved; });
			for (std::size_t i = 0; i < techs.size() && i < 12; ++i) {
				logger::info("[Instancing-v2]     technique {:8X}: {:6.0f} draws/frame, instancing saves {:6.0f}", techs[i].first, techs[i].second.draws / f, techs[i].second.saved / f);
			}
			g_techStats.clear();
			w = {};
		}

		struct SunShadowRender
		{
			static void thunk(RE::BSShadowDirectionalLight* a_this, std::uint32_t& a_index)
			{
				ZoneScopedN("Sonne Render");
				Stats::Count(Stats::Counter::SunRenderCalls);
				Stats::ScopedTimer timer{ Stats::Zone::SunShadowRender };
				g_inSunShadows.store(true, std::memory_order_relaxed);
				const auto start = std::chrono::steady_clock::now();
				func(a_this, a_index);
				g_sunRenderNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
				g_inSunShadows.store(false, std::memory_order_relaxed);
				ShadowCulling::AfterSunRender();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct SunShadowAccumulate
		{
			static void thunk(RE::BSShadowDirectionalLight* a_this, std::uint32_t& a_globalShadowLightCount, std::uint32_t a_shadowMaskChannel, RE::NiAVObject* a_cullingScene, std::uint8_t a_vrUpdateFlag)
			{
				ZoneScopedN("Sonne Accumulate");
				Stats::ScopedTimer timer{ Stats::Zone::SunShadowAccumulate };
				ShadowCulling::BeforeSunAccumulate(a_this);
				ShadowCulling::SetInSunAccumulate(true);
				func(a_this, a_globalShadowLightCount, a_shadowMaskChannel, a_cullingScene, a_vrUpdateFlag);
				ShadowCulling::SetInSunAccumulate(false);
				ShadowCulling::AfterSunAccumulate(a_this);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct SunUpdateCamera
		{
			static bool thunk(RE::BSShadowDirectionalLight* a_this, const RE::NiCamera* a_viewCamera)
			{
				const bool r = func(a_this, a_viewCamera);
				ShadowCulling::AfterSunUpdateCamera(a_this);
				return r;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		template <std::uint32_t Site>
		struct RenderPassImmediately
		{
			static void thunk(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags)
			{
				if (a_pass && ShadowCulling::ShouldSkipDepthPrepassDraw(*a_pass)) {
					return;  // Tiefenvorpass: kleines, fernes Objekt -> Draw ueberspringen
				}
				bool first = false;
				if (Site == 1 && g_instBatchActive && a_pass && Qualifies(*a_pass, a_alphaTest)) {
					if (CollectInstance(*a_pass)) {
						return;  // wird am Ende des Batches per Instancing gezeichnet
					}
					first = !g_instCaptured && Config::shadowInstancing.debugMode != 3;
				}
				const auto prev = t_callSite;
				t_callSite = Site;
				func(a_pass, a_technique, a_alphaTest, a_renderFlags);
				t_callSite = prev;
				if (first) {
					// Zustand des ersten normal gezeichneten Meshes merken (Pixel-Shader, Kamera, Raster-/Tiefenzustand)
					InstancedDraw::CaptureState(D3DContext());
					g_instCaptured = true;
					DebugCompareWorld(*a_pass);
				}
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
				Stats::Count(Stats::Counter::UtilityDraws);
				if (ShadowCulling::InDepthPrepass()) {
					Stats::Count(Stats::Counter::DepthDrawsAll);
				}
				if (g_inSunShadows.load(std::memory_order_relaxed) && a_pass && a_pass->geometry) {
					Stats::Count(Stats::Counter::SunDraws);
					if (Config::analysis.load(std::memory_order_relaxed)) {
						Record(*a_pass);
						Record2(*a_pass);
					}
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
					g_topGroups.emplace_back(g.count, std::format("{} (technique {:X})", g.sampleName ? g.sampleName : "?", key.technique));
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
			logger::info("[Instancing-Analysis] {} frames | sun shadow draws/frame {:.0f} | skinned {:.0f} | no TriShape {:.0f} | no buffer {:.0f}",
				w.frames, w.passes / f, w.skinned / f, w.nonTriShape / f, w.noRendererData / f);
			logger::info("[Instancing-Analysis]   call site: RenderBatches {:.0f} | second site {:.0f} | unknown {:.0f}",
				w.callSites[1] / f, w.callSites[2] / f, w.callSites[0] / f);
			logger::info("[Instancing-Analysis]   mesh groups/frame {:.0f} | draws with instancing from 2: {:.0f} ({:.0f}%) | from 4: {:.0f} ({:.0f}%) | from 8: {:.0f} ({:.0f}%)",
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
			logger::info("[Instancing-Analysis]   geometry types/frame:{}", types);
			for (const auto& [count, name] : g_topGroups) {
				logger::info("[Instancing-Analysis]     {:5}x  {}", count, name);
			}
			w = {};
		}

		std::uint32_t g_frameCounter = 0;
	}

	void Install()
	{
		REL::Relocation<std::uintptr_t> sunVtbl{ RE::VTABLE_BSShadowDirectionalLight[0] };
		SunShadowRender::func = sunVtbl.write_vfunc(0xA, SunShadowRender::thunk);
		SunShadowAccumulate::func = sunVtbl.write_vfunc(0x9, SunShadowAccumulate::thunk);
		SunUpdateCamera::func = sunVtbl.write_vfunc(0x10, SunUpdateCamera::thunk);
		logger::info("Hooks installed: BSShadowDirectionalLight::Accumulate (0x9) / Render (0xA)");

		RenderBatches::func = reinterpret_cast<decltype(RenderBatches::func)>(REL::RelocationID(100852, 107642).address());
		if (const auto err = DetourHelper::Attach(reinterpret_cast<void**>(&RenderBatches::func), reinterpret_cast<void*>(&RenderBatches::thunk)); err != 0) {
			Features::Report("Shadow instancing", "Gleiche Schatten bündeln", false, std::format("Detours error {} at RenderBatches", err));
		} else {
			logger::info("Detour installed: BSBatchRenderer::RenderBatches (instancing analysis, batch boundaries)");
			Features::Report("Shadow instancing", "Gleiche Schatten bündeln", !REL::Module::IsSE(), REL::Module::IsSE() ? "Anniversary Edition only" : "", REL::Module::IsSE());
		}

		REL::Relocation<std::uintptr_t> utilVtbl{ RE::VTABLE_BSUtilityShader[0] };
		UtilitySetupGeometry::func = utilVtbl.write_vfunc(0x6, UtilitySetupGeometry::thunk);
		logger::info("Hook installed: BSUtilityShader::SetupGeometry (vfunc 0x6)");

		// Aufrufstellen von RenderPassImmediately (dieselben wie Community Shaders/LightLimitFix) - nur zur Zuordnung
		auto& trampoline = SKSE::GetTrampoline();
		// Nur Diagnose-Zuordnung: ohne passenden call (andere Version/Mod) einfach weglassen
		REL::Relocation<std::uintptr_t> site1{ RELOCATION_ID(100852, 107642), REL::Relocate(0x29E, 0x28F) };
		REL::Relocation<std::uintptr_t> site2{ RELOCATION_ID(100877, 107667), REL::Relocate(0x1E5, 0xED) };
		const bool ok = Features::IsCall(site1.address()) && Features::IsCall(site2.address());
		if (ok) {
			RenderPassImmediately<1>::func = trampoline.write_call<5>(site1.address(), RenderPassImmediately<1>::thunk);
			RenderPassImmediately<2>::func = trampoline.write_call<5>(site2.address(), RenderPassImmediately<2>::thunk);
			logger::info("Hooks installed: RenderPassImmediately call sites 1 and 2");
		}
		Features::Report("Diagnostics: draw call assignment", "Diagnose: Draw-Zuordnung", ok, ok ? "" : "call sites differ");
	}

	void OnFrame()
	{
		EndFrame();
		EndFrame2();
		if (++g_frameCounter % kReportFrames == 0) {
			Report();
			Report2(g_sunRenderNs / 1e6 / kReportFrames);
			g_debugCompareBudget = 3;
			std::uint32_t mism = 0, tot = 0;
			InstancedDraw::DebugFlushStats(mism, tot);
			if (tot) {
				logger::info("[Instancing-Debug] flushes {} | of which with a different depth target than the first draw: {}", tot, mism);
			}
			g_sunRenderNs = 0;
		}
	}
}
