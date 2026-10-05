#include "TextureStream.h"

#include "Config.h"

namespace TextureStream
{
	namespace
	{
		// Rohdaten einer Textur, abgesichert gelesen (siehe SafeGather)
		struct RawTex
		{
			void*          texture;  // BSGraphics::Texture* (nur als Schluessel)
			const char*    name;
			std::uint16_t  width;
			std::uint16_t  height;
			std::uint8_t   mips;
			std::uint8_t   format;
		};

		struct Sample
		{
			void*         texture;
			std::string   name;
			std::uint16_t width, height;
			std::uint8_t  mips, format;
			float         neededPx;  // benoetigte Kantenlaenge in Texeln
		};

		// Texturen eines Materials lesen, ohne D3D-Objekte anzufassen. SEH-geschuetzt: ein ungueltiger Zeiger
		// (Textur wird gerade geladen/entladen) fuehrt nur zum Ueberspringen, nicht zum Absturz.
		int SafeGather(RE::BSLightingShaderMaterialBase* a_material, RawTex* a_out, int a_max) noexcept
		{
			__try {
				RE::NiSourceTexture* textures[16]{};
				std::uint32_t        count = a_material->GetTextures(textures);
				if (count > 16) {
					count = 16;
				}
				int n = 0;
				for (std::uint32_t i = 0; i < count && n < a_max; ++i) {
					const auto src = textures[i];
					if (!src || !src->rendererTexture) {
						continue;
					}
					const auto r = src->rendererTexture;
					a_out[n].texture = r;
					a_out[n].name = src->name.data();
					a_out[n].width = r->width;
					a_out[n].height = r->height;
					a_out[n].mips = r->mips;
					a_out[n].format = r->format;
					++n;
				}
				return n;
			} __except (1) {
				return -1;
			}
		}

		std::atomic<bool>   g_sampling{ false };  // dieser Frame wird ausgewertet
		std::mutex          g_lock;
		std::vector<Sample> g_samples;
		float               g_camX = 0, g_camY = 0, g_camZ = 0;
		float               g_pixelsPerUnitAtDistance1 = 1000.0f;  // Bildschirmhoehe / (2 * tan(FOV/2))

		std::chrono::steady_clock::time_point g_lastSample{};
		std::chrono::steady_clock::time_point g_lastReport{};

		struct TexStats
		{
			std::string   name;  // Kopie (Texturen koennen bis zum Bericht entladen werden)
			std::uint32_t width = 0, height = 0, mips = 0;
			float         bytesPerPixel = 0;
			float         neededPx = 0;  // Maximum ueber alle Mess-Frames im Berichtszeitraum
		};
		std::unordered_map<void*, TexStats> g_window;  // nur Main-Thread
		std::uint32_t                                          g_frames = 0;

		float BytesPerPixel(std::uint32_t a_format) noexcept
		{
			switch (a_format) {
			case 70: case 71: case 72:  // BC1
			case 79: case 80: case 81:  // BC4
				return 0.5f;
			case 73: case 74: case 75:  // BC2
			case 76: case 77: case 78:  // BC3
			case 82: case 83: case 84:  // BC5
			case 94: case 95: case 96:  // BC6H
			case 97: case 98: case 99:  // BC7
				return 1.0f;
			case 28: case 29: case 87: case 88: case 91: case 24: case 10:  // RGBA8/BGRA8/RGB10A2
				return 4.0f;
			case 61: case 62:  // R8
				return 1.0f;
			case 49: case 50:  // R8G8
				return 2.0f;
			default:
				return 4.0f;
			}
		}

		// Speicher einer Textur mit allen Mips, wenn ihre groesste Kante auf a_maxEdge begrenzt wird
		double SizeBytes(const TexStats& a_t, std::uint32_t a_maxEdge) noexcept
		{
			double w = a_t.width, h = a_t.height;
			while (std::max(w, h) > a_maxEdge && std::max(w, h) > 4) {
				w /= 2;
				h /= 2;
			}
			return w * h * a_t.bytesPerPixel * (a_t.mips > 1 ? 4.0 / 3.0 : 1.0);
		}

		std::uint32_t NextPow2(float a_v) noexcept
		{
			std::uint32_t p = 4;
			while (p < a_v && p < 16384) {
				p <<= 1;
			}
			return p;
		}

		void Report()
		{
			if (g_window.empty()) {
				return;
			}
			const float safety = Config::textureStream.safetyFactor;
			double      full = 0, needed = 0;
			struct Row
			{
				const TexStats* t;
				double          saving;
				std::uint32_t   needEdge;
			};
			std::vector<Row> rows;
			rows.reserve(g_window.size());
			std::uint32_t big = 0, noMips = 0;
			double        noMipsBytes = 0;
			for (const auto& [tex, t] : g_window) {
				const std::uint32_t edge = std::max(t.width, t.height);
				const std::uint32_t needEdge = std::min(edge, NextPow2(t.neededPx * safety));
				const double        f = SizeBytes(t, edge);
				const double        n = SizeBytes(t, needEdge);
				full += f;
				needed += n;
				if (edge >= 4096) {
					++big;
				}
				// ohne Mip-Kette laesst sich eine Textur nicht einfach per Kopie verkleinern -> nicht einrechnen
				if (t.mips <= 1 && edge >= 512) {
					++noMips;
					noMipsBytes += f;
					needed += f - n;
					continue;
				}
				rows.push_back({ &t, f - n, needEdge });
			}
			std::ranges::sort(rows, [](const Row& a, const Row& b) { return a.saving > b.saving; });
			logger::info("[TextureStream] {} Mess-Frames | sichtbare Texturen {} (davon 4K+ {}) | VRAM voll {:.0f} MB | benoetigt (Sicherheit x{:.1f}) {:.0f} MB | Ersparnis {:.0f} MB ({:.0f} %)",
				g_frames, g_window.size(), big, full / 1048576.0, safety, needed / 1048576.0, (full - needed) / 1048576.0, full > 0 ? 100.0 * (full - needed) / full : 0.0);
			if (noMips) {
				logger::info("[TextureStream]   ohne Mip-Kette (ab 512, nicht verkleinerbar): {} Texturen, {:.0f} MB", noMips, noMipsBytes / 1048576.0);
			}
			for (std::size_t i = 0; i < rows.size() && i < 12; ++i) {
				const auto& r = rows[i];
				logger::info("[TextureStream]   {:5.1f} MB sparen | {}x{} -> {} | {}", r.saving / 1048576.0, r.t->width, r.t->height, r.needEdge, r.t->name);
			}
		}
	}

	void OnGeometry(RE::BSGeometry* a_geometry, RE::BSLightingShaderProperty* a_property) noexcept
	{
		if (!g_sampling.load(std::memory_order_relaxed) || !a_geometry || !a_property) {
			return;
		}
		const auto material = static_cast<RE::BSLightingShaderMaterialBase*>(a_property->material);
		if (!material) {
			return;
		}
		// Bildschirm-Durchmesser des Objekts in Pixeln (naeher als sein Radius -> volle Aufloesung)
		const auto& b = a_geometry->worldBound;
		const float dx = b.center.x - g_camX, dy = b.center.y - g_camY, dz = b.center.z - g_camZ;
		const float dist = std::sqrt(dx * dx + dy * dy + dz * dz) - b.radius;
		const float neededPx = dist <= 1.0f ? 1.0e6f : 2.0f * b.radius / dist * g_pixelsPerUnitAtDistance1;

		RawTex    raw[16];
		const int count = SafeGather(material, raw, 16);
		if (count <= 0) {
			return;
		}
		std::scoped_lock lock(g_lock);
		for (int i = 0; i < count; ++i) {
			const auto& r = raw[i];
			if (r.width == 0 || r.height == 0) {
				continue;
			}
			g_samples.push_back({ r.texture, r.name ? std::string(r.name) : std::string(), r.width, r.height, r.mips, r.format, neededPx });
		}
	}

	void OnFrame()
	{
		const auto& cfg = Config::textureStream;
		const auto  now = std::chrono::steady_clock::now();

		// Ergebnisse des letzten Mess-Frames uebernehmen (Main-Thread)
		if (g_sampling.exchange(false)) {
			std::vector<Sample> samples;
			{
				std::scoped_lock lock(g_lock);
				samples.swap(g_samples);
			}
			++g_frames;
			for (const auto& s : samples) {
				auto& t = g_window[s.texture];
				if (t.width == 0) {
					t.name = s.name;
					t.width = s.width;
					t.height = s.height;
					t.mips = s.mips;
					t.bytesPerPixel = BytesPerPixel(s.format);
				}
				t.neededPx = std::max(t.neededPx, s.neededPx);
			}
		}

		if (!cfg.analysis) {
			g_window.clear();
			return;
		}
		if (now - g_lastReport >= std::chrono::seconds(10)) {
			g_lastReport = now;
			Report();
			g_window.clear();
			g_frames = 0;
		}
		// naechsten Frame messen? Nicht in Ladebildschirm/Menues (Texturen werden dort geladen/entladen)
		const auto ui = RE::UI::GetSingleton();
		if (!ui || ui->GameIsPaused() || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
			return;
		}
		if (now - g_lastSample >= std::chrono::milliseconds(1000)) {
			g_lastSample = now;
			if (const auto camera = RE::PlayerCamera::GetSingleton(); camera && camera->cameraRoot) {
				const auto& pos = camera->cameraRoot->world.translate;
				g_camX = pos.x;
				g_camY = pos.y;
				g_camZ = pos.z;
			}
			float tanHalf = 0.6f;
			if (const auto cam = RE::Main::WorldRootCamera()) {
				const float top = cam->GetRuntimeData2().viewFrustum.fTop;
				if (top > 0.01f && top < 5.0f) {
					tanHalf = top;
				}
			}
			float screenH = 1200.0f;
			if (const auto state = RE::BSGraphics::State::GetSingleton(); state && state->screenHeight > 0) {
				screenH = static_cast<float>(state->screenHeight);
			}
			g_pixelsPerUnitAtDistance1 = screenH / (2.0f * tanHalf);
			g_sampling.store(true, std::memory_order_relaxed);
		}
	}
}
