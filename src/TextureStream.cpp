#include "TextureStream.h"

#include "Config.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace TextureStream
{
	namespace
	{
		namespace W = REX::W32;
		using Clock = std::chrono::steady_clock;

		// ---------------- Formate / DDS ----------------

		struct FormatInfo
		{
			bool          bc = false;  // Blockkompression: bytes = Bytes je 4x4-Block, sonst Bytes je Pixel
			std::uint32_t bytes = 0;   // 0 = unbekannt (nie anfassen)

			bool operator==(const FormatInfo&) const = default;
		};

		FormatInfo InfoOf(std::uint32_t a_dxgi) noexcept
		{
			switch (a_dxgi) {
			case 70: case 71: case 72:  // BC1
			case 79: case 80: case 81:  // BC4
				return { true, 8 };
			case 73: case 74: case 75:  // BC2
			case 76: case 77: case 78:  // BC3
			case 82: case 83: case 84:  // BC5
			case 94: case 95: case 96:  // BC6H
			case 97: case 98: case 99:  // BC7
				return { true, 16 };
			case 23: case 24: case 25:                                // R10G10B10A2
			case 27: case 28: case 29: case 30: case 31: case 32:     // R8G8B8A8
			case 87: case 88: case 90: case 91: case 92: case 93:     // B8G8R8A8 / X8
				return { false, 4 };
			case 9: case 10: case 11: case 12: case 13: case 14:  // R16G16B16A16
				return { false, 8 };
			case 48: case 49: case 50: case 51: case 52:              // R8G8
			case 53: case 54: case 55: case 56: case 57: case 58:     // R16
			case 85: case 86: case 115:                               // B5G6R5 / B5G5R5A1 / B4G4R4A4
				return { false, 2 };
			case 60: case 61: case 62: case 63: case 64: case 65:  // R8 / A8
				return { false, 1 };
			default:
				return {};
			}
		}

		std::uint64_t MipBytes(FormatInfo a_fi, std::uint32_t a_w, std::uint32_t a_h) noexcept
		{
			if (a_fi.bc) {
				return std::uint64_t(std::max(1u, (a_w + 3) / 4)) * std::max(1u, (a_h + 3) / 4) * a_fi.bytes;
			}
			return std::uint64_t(a_w) * a_h * a_fi.bytes;
		}

		std::uint32_t RowPitch(FormatInfo a_fi, std::uint32_t a_w) noexcept
		{
			return a_fi.bc ? std::max(1u, (a_w + 3) / 4) * a_fi.bytes : a_w * a_fi.bytes;
		}

		// Speicher aller Mip-Stufen ab Groesse w x h
		std::uint64_t ChainBytes(FormatInfo a_fi, std::uint32_t a_w, std::uint32_t a_h, std::uint32_t a_mips) noexcept
		{
			std::uint64_t sum = 0;
			for (std::uint32_t i = 0; i < a_mips; ++i) {
				sum += MipBytes(a_fi, std::max(1u, a_w >> i), std::max(1u, a_h >> i));
			}
			return sum;
		}

		constexpr std::uint32_t FourCC(char a, char b, char c, char d) { return std::uint32_t(std::uint8_t(a)) | (std::uint32_t(std::uint8_t(b)) << 8) | (std::uint32_t(std::uint8_t(c)) << 16) | (std::uint32_t(std::uint8_t(d)) << 24); }

		struct DDSPixelFormat
		{
			std::uint32_t size, flags, fourCC, rgbBitCount, rMask, gMask, bMask, aMask;
		};
		struct DDSHeader
		{
			std::uint32_t  size, flags, height, width, pitch, depth, mipCount;
			std::uint32_t  reserved1[11];
			DDSPixelFormat pf;
			std::uint32_t  caps, caps2, caps3, caps4, reserved2;
		};
		static_assert(sizeof(DDSHeader) == 124);
		struct DDSHeaderDX10
		{
			std::uint32_t dxgiFormat, resourceDimension, miscFlag, arraySize, miscFlags2;
		};

		struct FileInfo
		{
			std::uint32_t width = 0, height = 0, mips = 0;
			FormatInfo    fi;
			std::uint32_t dataOffset = 0;  // Beginn der Pixeldaten
		};

		// Oeffnet die Datei (lose oder aus BSA) und liest den Kopf. Leerer Fehlertext = ok.
		std::string ReadHeader(RE::BSResourceNiBinaryStream& a_stream, FileInfo& a_out)
		{
			if (!a_stream.good()) {
				return "Datei nicht gefunden";
			}
			std::uint32_t magic = 0;
			DDSHeader     h{};
			if (!a_stream.read(&magic, 1) || magic != FourCC('D', 'D', 'S', ' ') || !a_stream.read(&h, 1) || h.size != 124) {
				return "kein DDS";
			}
			a_out.width = h.width;
			a_out.height = h.height;
			a_out.mips = std::max(1u, h.mipCount);
			a_out.dataOffset = 4 + 124;
			if (h.caps2 & 0x200) {
				return "Cubemap";
			}
			if (h.depth > 1 && (h.caps2 & 0x200000)) {
				return "Volumentextur";
			}
			const auto& pf = h.pf;
			if (pf.flags & 0x4) {  // FourCC
				switch (pf.fourCC) {
				case FourCC('D', 'X', 'T', '1'):
				case FourCC('A', 'T', 'I', '1'):
				case FourCC('B', 'C', '4', 'U'):
				case FourCC('B', 'C', '4', 'S'):
					a_out.fi = { true, 8 };
					break;
				case FourCC('D', 'X', 'T', '2'):
				case FourCC('D', 'X', 'T', '3'):
				case FourCC('D', 'X', 'T', '4'):
				case FourCC('D', 'X', 'T', '5'):
				case FourCC('A', 'T', 'I', '2'):
				case FourCC('B', 'C', '5', 'U'):
				case FourCC('B', 'C', '5', 'S'):
					a_out.fi = { true, 16 };
					break;
				case FourCC('D', 'X', '1', '0'):
					{
						DDSHeaderDX10 dx{};
						if (!a_stream.read(&dx, 1)) {
							return "DX10-Kopf fehlt";
						}
						a_out.dataOffset += sizeof(DDSHeaderDX10);
						if (dx.resourceDimension != 3 || dx.arraySize > 1 || (dx.miscFlag & 0x4)) {
							return "kein einfaches 2D-Bild";
						}
						a_out.fi = InfoOf(dx.dxgiFormat);
						break;
					}
				default:
					return std::format("FourCC 0x{:08X} unbekannt", pf.fourCC);
				}
			} else if (pf.flags & (0x40 | 0x20000 | 0x2)) {  // RGB / Luminanz / Alpha
				a_out.fi = { false, pf.rgbBitCount / 8 };
			} else {
				return "Pixelformat unbekannt";
			}
			if (a_out.fi.bytes == 0) {
				return "Format nicht unterstuetzt";
			}
			return {};
		}

		// ---------------- Zustand je Textur (nur Main-Thread) ----------------

		enum class Probe : std::uint8_t
		{
			kNone,
			kPending,
			kOk,
			kBad
		};

		struct TexState
		{
			RE::NiPointer<RE::NiSourceTexture> hold;  // haelt die Textur am Leben, solange wir sie veraendert haben / bearbeiten
			W::ID3D11Resource*                 res = nullptr;  // aktuelles D3D-Objekt (anderes = Spiel hat neu geladen)
			std::string                        path;           // BSResource-Pfad ("textures\...")
			std::uint32_t                      fullW = 0, fullH = 0, fullMips = 0;  // Original (laut Datei)
			std::uint32_t                      curW = 0, curH = 0, curMips = 0;     // jetzt im VRAM
			std::uint32_t                      format = 0;
			FormatInfo                         fi;
			Probe                              probe = Probe::kNone;
			bool                               eligible = false;
			bool                               busy = false;  // Auftrag laeuft
			std::uint32_t                      passId = 0;    // zuletzt gesehen in Durchlauf
			float                              passNeed = 0;  // max. Bildschirmgroesse (px) im Durchlauf
			std::uint32_t                      lowPasses = 0;
			std::uint32_t                      lowTarget = 0;
			Clock::time_point                  lastSeen{};

			std::uint32_t FullEdge() const noexcept { return std::max(fullW, fullH); }
			std::uint32_t CurEdge() const noexcept { return std::max(curW, curH); }
			bool          Reduced() const noexcept { return CurEdge() < FullEdge(); }
		};

		std::unordered_map<RE::BSGraphics::Texture*, TexState> g_tex;

		W::ID3D11Device*        g_device = nullptr;
		W::ID3D11DeviceContext* g_context = nullptr;

		// ---------------- Statistik ----------------
		struct Counters
		{
			std::uint32_t downs = 0, ups = 0, upFails = 0, probesBad = 0;
			double        upMs = 0, upMB = 0;
			std::uint32_t passes = 0, passFrames = 0, passNodes = 0;
			double        passMs = 0;
		} g_stats;
		std::uint32_t g_failLogged = 0;

		// ---------------- verzoegerte Freigabe (Render-Jobs koennten den alten Zeiger noch halten) ----------------
		// Grosse Texturen gebuendelt freizugeben liess den Treiber haengen (0.18.3: 180-255-ms-Frames bei Wellen von
		// >1000 Verkleinerungen) -> hoechstens kReleaseBytesPerCall pro Aufruf, aelteste zuerst.
		struct Deferred
		{
			Clock::time_point time;
			W::IUnknown*      obj;
			std::uint64_t     bytes;
		};
		std::deque<Deferred>    g_deferred;
		constexpr std::uint64_t kReleaseBytesPerCall = 64ull << 20;

		void DeferRelease(W::IUnknown* a_obj, std::uint64_t a_bytes = 0)
		{
			if (a_obj) {
				g_deferred.push_back({ Clock::now(), a_obj, a_bytes });
			}
		}

		void ProcessDeferred()
		{
			const auto    now = Clock::now();
			std::uint64_t released = 0;
			while (!g_deferred.empty() && now - g_deferred.front().time >= 500ms) {
				const auto& e = g_deferred.front();
				if (released > 0 && released + e.bytes > kReleaseBytesPerCall) {
					break;
				}
				released += e.bytes;
				e.obj->Release();
				g_deferred.pop_front();
			}
		}

		void Swap(RE::BSGraphics::Texture* a_r, W::ID3D11Texture2D* a_tex, W::ID3D11ShaderResourceView* a_srv, std::uint64_t a_oldBytes)
		{
			DeferRelease(a_r->texture, a_oldBytes);
			DeferRelease(a_r->resourceView);
			a_r->texture = a_tex;
			a_r->resourceView = a_srv;
		}

		// ---------------- Hintergrund-Thread: Kopf pruefen / neu laden ----------------

		struct Job
		{
			bool                               reload = false;  // false = nur Kopf pruefen
			RE::BSGraphics::Texture*           r = nullptr;
			W::ID3D11Resource*                 expectRes = nullptr;
			std::string                        path;
			std::uint32_t                      fullW = 0, fullH = 0, fullMips = 0, format = 0;
			FormatInfo                         fi;
			std::uint32_t                      skip = 0;  // obere Mip-Stufen weglassen
			W::D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		};

		struct Result
		{
			Job                          job;
			std::string                  error;
			FileInfo                     file;
			W::ID3D11Texture2D*          tex = nullptr;
			W::ID3D11ShaderResourceView* srv = nullptr;
			double                       ms = 0;
			double                       mb = 0;
		};

		std::mutex              g_qLock;
		std::condition_variable g_qCv;
		std::deque<Job>         g_jobs;
		std::vector<Result>     g_results;
		std::atomic<bool>       g_workerStarted{ false };

		bool SkipBytes(RE::BSResourceNiBinaryStream& a_s, std::uint64_t a_bytes)
		{
			const auto pos = a_s.tell();
			a_s.seek(static_cast<std::int32_t>(a_bytes));
			return a_s.tell() == pos + a_bytes;
		}

		bool DiscardBytes(RE::BSResourceNiBinaryStream& a_s, std::uint64_t a_bytes)
		{
			std::vector<std::byte> buf(std::min<std::uint64_t>(a_bytes, 8u << 20));
			while (a_bytes > 0) {
				const auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(a_bytes, buf.size()));
				if (!a_s.read(buf.data(), n)) {
					return false;
				}
				a_bytes -= n;
			}
			return true;
		}

		void RunJob(Result& a_res)
		{
			const auto& job = a_res.job;
			const auto  t0 = Clock::now();
			auto        stream = std::make_unique<RE::BSResourceNiBinaryStream>(job.path);
			a_res.error = ReadHeader(*stream, a_res.file);
			if (!a_res.error.empty() || !job.reload) {
				return;
			}
			const auto& f = a_res.file;
			if (f.width != job.fullW || f.height != job.fullH || f.mips != job.fullMips || !(f.fi == job.fi)) {
				a_res.error = "Datei hat sich geaendert";
				return;
			}
			std::uint64_t skipBytes = 0;
			for (std::uint32_t i = 0; i < job.skip; ++i) {
				skipBytes += MipBytes(f.fi, std::max(1u, f.width >> i), std::max(1u, f.height >> i));
			}
			if (skipBytes > 0 && !SkipBytes(*stream, skipBytes)) {
				// Springen nicht moeglich (z. B. komprimiertes Archiv) -> neu oeffnen und ueberlesen
				stream = std::make_unique<RE::BSResourceNiBinaryStream>(job.path);
				FileInfo again;
				if (!ReadHeader(*stream, again).empty() || !DiscardBytes(*stream, skipBytes)) {
					a_res.error = "Lesefehler beim Ueberspringen";
					return;
				}
			}
			const std::uint32_t w = std::max(1u, f.width >> job.skip), h = std::max(1u, f.height >> job.skip);
			const std::uint32_t mips = f.mips - job.skip;
			const auto          bytes = ChainBytes(f.fi, w, h, mips);
			std::vector<std::byte> data(bytes);
			for (std::uint64_t done = 0; done < bytes;) {
				const auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(bytes - done, 16u << 20));
				if (!stream->read(data.data() + done, n)) {
					a_res.error = "Datei zu kurz";
					return;
				}
				done += n;
			}
			stream.reset();

			std::vector<W::D3D11_SUBRESOURCE_DATA> init(mips);
			std::uint64_t                          off = 0;
			for (std::uint32_t i = 0; i < mips; ++i) {
				const std::uint32_t mw = std::max(1u, w >> i), mh = std::max(1u, h >> i);
				init[i].sysMem = data.data() + off;
				init[i].sysMemPitch = RowPitch(f.fi, mw);
				init[i].sysMemSlicePitch = static_cast<std::uint32_t>(MipBytes(f.fi, mw, mh));
				off += MipBytes(f.fi, mw, mh);
			}
			W::D3D11_TEXTURE2D_DESC desc{};
			desc.width = w;
			desc.height = h;
			desc.mipLevels = mips;
			desc.arraySize = 1;
			desc.format = static_cast<W::DXGI_FORMAT>(job.format);
			desc.sampleDesc.count = 1;
			desc.usage = W::D3D11_USAGE_IMMUTABLE;
			desc.bindFlags = W::D3D11_BIND_SHADER_RESOURCE;
			if (g_device->CreateTexture2D(&desc, init.data(), &a_res.tex) < 0 || !a_res.tex) {
				a_res.error = "CreateTexture2D fehlgeschlagen";
				return;
			}
			auto sd = job.srvDesc;
			sd.texture2D.mostDetailedMip = 0;
			sd.texture2D.mipLevels = static_cast<std::uint32_t>(-1);
			if (g_device->CreateShaderResourceView(a_res.tex, &sd, &a_res.srv) < 0 || !a_res.srv) {
				a_res.tex->Release();
				a_res.tex = nullptr;
				a_res.error = "CreateShaderResourceView fehlgeschlagen";
				return;
			}
			a_res.mb = bytes / 1048576.0;
			a_res.ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
		}

		void ApplyResults();
		void PreviewTick();

		void Worker()
		{
			auto lastTick = Clock::now();
			for (;;) {
				Job job;
				bool have = false;
				{
					std::unique_lock lock(g_qLock);
					g_qCv.wait_for(lock, 250ms, [] { return !g_jobs.empty(); });
					if (!g_jobs.empty()) {
						job = std::move(g_jobs.front());
						g_jobs.pop_front();
						have = true;
					}
				}
				if (have) {
					Result res;
					res.job = std::move(job);
					try {
						RunJob(res);
					} catch (const std::exception& e) {
						res.error = e.what();
					}
					{
						std::scoped_lock lock(g_qLock);
						g_results.push_back(std::move(res));
					}
					if (const auto tasks = SKSE::GetTaskInterface()) {
						tasks->AddTask([] { ApplyResults(); });
					}
				}
				// Vorschau-Menues pruefen (laeuft per SKSE-Task auch, wenn das Spiel pausiert)
				if (Clock::now() - lastTick >= 250ms) {
					lastTick = Clock::now();
					if (const auto tasks = SKSE::GetTaskInterface()) {
						tasks->AddTask([] { PreviewTick(); });
					}
				}
			}
		}

		void Enqueue(Job a_job)
		{
			if (!g_workerStarted.exchange(true)) {
				std::thread(Worker).detach();
			}
			{
				std::scoped_lock lock(g_qLock);
				g_jobs.push_back(std::move(a_job));
			}
			g_qCv.notify_one();
		}

		// ---------------- Main-Thread-Logik ----------------

		std::vector<std::string> g_excludeTokens;
		std::string              g_excludeSource;

		bool Excluded(const std::string& a_path)
		{
			const auto& cfg = Config::textureStream.exclude;
			if (cfg != g_excludeSource) {
				g_excludeSource = cfg;
				g_excludeTokens.clear();
				std::size_t pos = 0;
				while (pos <= cfg.size()) {
					const auto end = cfg.find(',', pos);
					auto       tok = cfg.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
					std::ranges::transform(tok, tok.begin(), [](char c) { return c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
					std::erase(tok, ' ');
					if (!tok.empty()) {
						g_excludeTokens.push_back(std::move(tok));
					}
					if (end == std::string::npos) {
						break;
					}
					pos = end + 1;
				}
			}
			return std::ranges::any_of(g_excludeTokens, [&](const std::string& t) { return a_path.find(t) != std::string::npos; });
		}

		bool Active() noexcept
		{
			return Config::textureStream.enabled && Config::masterEnabled.load(std::memory_order_relaxed);
		}

		// benoetigte Kantenlaenge fuer eine Bildschirmgroesse (Zweierpotenz, zwischen fMinEdge und Original)
		std::uint32_t WantedEdge(const TexState& a_st, float a_needPx) noexcept
		{
			const auto full = a_st.FullEdge();
			if (!Active()) {
				return full;
			}
			const float   need = std::max(a_needPx * Config::textureStream.safetyFactor, Config::textureStream.minEdge);
			std::uint32_t p = 4;
			while (p < need && p < full) {
				p <<= 1;
			}
			return std::min(p, full);
		}

		bool ReadDesc(W::ID3D11Resource* a_res, W::D3D11_TEXTURE2D_DESC& a_desc) noexcept
		{
			__try {
				W::D3D11_RESOURCE_DIMENSION dim{};
				a_res->GetType(&dim);
				if (dim != W::D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
					return false;
				}
				static_cast<W::ID3D11Texture2D*>(a_res)->GetDesc(&a_desc);
				return true;
			} __except (1) {
				return false;
			}
		}

		std::string NormalizePath(const char* a_name)
		{
			std::string p = a_name ? a_name : "";
			std::ranges::transform(p, p.begin(), [](char c) { return c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
			if (p.starts_with("data\\")) {
				p.erase(0, 5);
			}
			return p;
		}

		// ---------------- Stufe 3: gemerkte Groessen fuer das Laden (Lade-Threads lesen, Main-Thread schreibt) ----------------

		std::mutex                                     g_sizeLock;
		std::unordered_map<std::string, std::uint32_t> g_loadEdge;  // Pfad -> max. Kantenlaenge beim Laden
		bool                                           g_sizesDirty = false;
		std::atomic<std::uint32_t>                     g_loadedReduced{ 0 };
		thread_local RE::NiSourceTexture*              t_loadingSrc = nullptr;

		std::filesystem::path SizesFile()
		{
			auto dir = SKSE::log::log_directory();
			return dir ? *dir / "SkyrimPerf_TextureSizes.txt" : std::filesystem::path{};
		}

		// a_edge 0 = vergessen (wird wieder voll geladen)
		void RememberEdge(const std::string& a_path, std::uint32_t a_edge)
		{
			if (a_path.empty()) {
				return;
			}
			std::scoped_lock lock(g_sizeLock);
			if (a_edge == 0) {
				g_sizesDirty |= g_loadEdge.erase(a_path) > 0;
			} else if (auto& e = g_loadEdge[a_path]; e != a_edge) {
				e = a_edge;
				g_sizesDirty = true;
			}
		}

		bool HasRememberedEdge(const std::string& a_path)
		{
			std::scoped_lock lock(g_sizeLock);
			return g_loadEdge.contains(a_path);
		}

		void LoadSizes()
		{
			std::ifstream in(SizesFile());
			std::string   line;
			std::size_t   n = 0;
			while (std::getline(in, line)) {
				const auto bar = line.find('|');
				if (bar == std::string::npos) {
					continue;
				}
				const auto edge = static_cast<std::uint32_t>(std::strtoul(line.c_str(), nullptr, 10));
				if (edge >= 64) {
					g_loadEdge[line.substr(bar + 1)] = edge;
					++n;
				}
			}
			logger::info("TextureStream: {} gemerkte Texturgroessen geladen", n);
		}

		void SaveSizes()
		{
			std::vector<std::pair<std::string, std::uint32_t>> copy;
			{
				std::scoped_lock lock(g_sizeLock);
				if (!g_sizesDirty) {
					return;
				}
				g_sizesDirty = false;
				copy.assign(g_loadEdge.begin(), g_loadEdge.end());
			}
			const auto    file = SizesFile();
			const auto    tmp = std::filesystem::path(file).concat(".tmp");
			std::ofstream out(tmp, std::ios::trunc);
			for (const auto& [path, edge] : copy) {
				out << edge << '|' << path << '\n';
			}
			out.close();
			std::error_code ec;
			std::filesystem::rename(tmp, file, ec);
		}

		// Lade-Thread: maxsize fuer die gerade geladene Textur (0 = unbegrenzt)
		std::uint64_t LoadMaxSize() noexcept
		{
			const auto src = t_loadingSrc;
			if (!src || !Config::textureStream.loadReduced || !Config::textureStream.enabled || !Config::masterEnabled.load(std::memory_order_relaxed)) {
				return 0;
			}
			try {
				const auto path = NormalizePath(src->name.c_str());
				std::scoped_lock lock(g_sizeLock);
				const auto it = g_loadEdge.find(path);
				if (it == g_loadEdge.end()) {
					return 0;
				}
				g_loadedReduced.fetch_add(1, std::memory_order_relaxed);
				return std::max<std::uint32_t>(it->second, static_cast<std::uint32_t>(Config::textureStream.minEdge));
			} catch (...) {
				return 0;
			}
		}

		// ID 108531 +0x44 ruft ID 77301 (Renderer-Textur aus Stream anlegen); r9 = NiSourceTexture + 0x20
		struct CreateRendererTexture
		{
			static void* thunk(void* a_1, void* a_stream, std::uint64_t a_flag, std::byte* a_srcPlus20)
			{
				t_loadingSrc = a_srcPlus20 ? reinterpret_cast<RE::NiSourceTexture*>(a_srcPlus20 - 0x20) : nullptr;
				const auto result = func(a_1, a_stream, a_flag, a_srcPlus20);
				t_loadingSrc = nullptr;
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// ID 77301 +0x62 ruft ID 77533 (DDS laden, DirectXTK) mit maxsize = 0
		struct LoadDDS
		{
			static std::int32_t thunk(void* a_device, void* a_stream, void** a_out, void* a_header, std::uint64_t a_maxSize, std::uint64_t a_6)
			{
				if (a_maxSize == 0) {
					a_maxSize = LoadMaxSize();
				}
				return func(a_device, a_stream, a_out, a_header, a_maxSize, a_6);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		void QueueReload(TexState& a_st, RE::BSGraphics::Texture* a_r, std::uint32_t a_targetEdge)
		{
			std::uint32_t skip = 0;
			while ((a_st.FullEdge() >> (skip + 1)) >= a_targetEdge && skip + 1 < a_st.fullMips) {
				++skip;
			}
			Job job;
			job.reload = true;
			job.r = a_r;
			job.expectRes = a_st.res;
			job.path = a_st.path;
			job.fullW = a_st.fullW;
			job.fullH = a_st.fullH;
			job.fullMips = a_st.fullMips;
			job.format = a_st.format;
			job.fi = a_st.fi;
			job.skip = skip;
			if (!a_r->resourceView) {
				return;
			}
			a_r->resourceView->GetDesc(&job.srvDesc);
			if (static_cast<int>(job.srvDesc.viewDimension) != 4) {  // TEXTURE2D
				a_st.eligible = false;
				return;
			}
			a_st.busy = true;
			Enqueue(std::move(job));
		}

		void QueueProbe(TexState& a_st, RE::BSGraphics::Texture* a_r)
		{
			Job job;
			job.r = a_r;
			job.expectRes = a_st.res;
			job.path = a_st.path;
			a_st.probe = Probe::kPending;
			a_st.busy = true;
			Enqueue(std::move(job));
		}

		std::uint32_t g_passId = 1;

		// Textur wurde an einem Objekt gefunden, das a_needPx Pixel gross erscheint
		void OnSeen(RE::NiSourceTexture* a_src, float a_needPx)
		{
			const auto r = a_src->rendererTexture;
			if (!r || !r->texture) {
				return;
			}
			auto [it, inserted] = g_tex.try_emplace(r);
			auto& st = it->second;
			if (inserted || st.res != r->texture) {
				// neu oder vom Spiel ersetzt: Zustand aus dem D3D-Objekt neu aufbauen
				const bool busy = st.busy;
				st = TexState{};
				st.busy = busy;
				st.res = r->texture;
				W::D3D11_TEXTURE2D_DESC d{};
				if (ReadDesc(st.res, d)) {
					st.curW = st.fullW = d.width;
					st.curH = st.fullH = d.height;
					st.curMips = st.fullMips = d.mipLevels;
					st.format = static_cast<std::uint32_t>(d.format);
					st.fi = InfoOf(st.format);
					st.path = NormalizePath(a_src->name.c_str());
					st.eligible = d.arraySize == 1 && d.sampleDesc.count == 1 && !(d.miscFlags & 0x4) && d.mipLevels > 1 && st.fi.bytes > 0 &&
					              st.path.starts_with("textures\\") && st.path.ends_with(".dds") && !Excluded(st.path);
					// Evtl. schon verkleinert geladen (Stufe 3) -> Originalgroesse sofort aus der Datei holen, damit
					// ein naheliegendes Objekt gleich wieder die volle Groesse bekommt
					if (st.eligible && !st.busy && HasRememberedEdge(st.path)) {
						QueueProbe(st, r);
					}
				}
			}
			st.lastSeen = Clock::now();
			if (st.passId != g_passId) {
				st.passId = g_passId;
				st.passNeed = 0;
			}
			st.passNeed = std::max(st.passNeed, a_needPx);
			if (!st.eligible) {
				return;
			}
			if (!st.hold) {
				st.hold.reset(a_src);
			}
			// zu klein fuer diesen Abstand -> sofort groesser laden (mit einer Stufe Reserve)
			if (st.Reduced() && !st.busy && st.probe == Probe::kOk) {
				const auto want = WantedEdge(st, a_needPx);
				if (want > st.CurEdge()) {
					QueueReload(st, r, std::min(st.FullEdge(), want * 2));
				}
			}
		}

		// ---------------- Verkleinern per GPU-Kopie (Main-Thread) ----------------

		struct DownJob
		{
			RE::BSGraphics::Texture*           r;
			RE::NiPointer<RE::NiSourceTexture> src;
			std::uint32_t                      targetEdge;
		};
		std::deque<DownJob> g_down;

		// a_freed: frei werdender Speicher (fuer das Budget pro Frame)
		void Downscale(const DownJob& a_job, std::uint64_t& a_freed)
		{
			a_freed = 0;
			const auto it = g_tex.find(a_job.r);
			if (it == g_tex.end()) {
				return;
			}
			auto& st = it->second;
			st.busy = false;
			const auto r = a_job.r;
			if (!a_job.src || a_job.src->rendererTexture != r || r->texture != st.res || !r->resourceView || !g_device || !g_context) {
				return;
			}
			std::uint32_t drop = 0;
			while ((st.CurEdge() >> (drop + 1)) >= a_job.targetEdge && drop + 1 < st.curMips) {
				++drop;
			}
			// Blockformate: oberste Stufe muss durch 4 teilbar sein
			while (drop > 0 && st.fi.bc && (((st.curW >> drop) % 4) != 0 || ((st.curH >> drop) % 4) != 0)) {
				--drop;
			}
			if (drop == 0) {
				return;
			}
			W::D3D11_TEXTURE2D_DESC desc{};
			desc.width = std::max(1u, st.curW >> drop);
			desc.height = std::max(1u, st.curH >> drop);
			desc.mipLevels = st.curMips - drop;
			desc.arraySize = 1;
			desc.format = static_cast<W::DXGI_FORMAT>(st.format);
			desc.sampleDesc.count = 1;
			desc.usage = W::D3D11_USAGE_DEFAULT;
			desc.bindFlags = W::D3D11_BIND_SHADER_RESOURCE;
			W::ID3D11Texture2D* tex = nullptr;
			if (g_device->CreateTexture2D(&desc, nullptr, &tex) < 0 || !tex) {
				st.eligible = false;
				return;
			}
			W::D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
			r->resourceView->GetDesc(&sd);
			if (static_cast<int>(sd.viewDimension) != 4) {
				tex->Release();
				st.eligible = false;
				return;
			}
			sd.texture2D.mostDetailedMip = 0;
			sd.texture2D.mipLevels = static_cast<std::uint32_t>(-1);
			W::ID3D11ShaderResourceView* srv = nullptr;
			if (g_device->CreateShaderResourceView(tex, &sd, &srv) < 0 || !srv) {
				tex->Release();
				st.eligible = false;
				return;
			}
			for (std::uint32_t i = 0; i < desc.mipLevels; ++i) {
				g_context->CopySubresourceRegion(tex, i, 0, 0, 0, st.res, i + drop, nullptr);
			}
			const auto oldBytes = ChainBytes(st.fi, st.curW, st.curH, st.curMips);
			Swap(r, tex, srv, oldBytes);
			a_freed = oldBytes - ChainBytes(st.fi, desc.width, desc.height, desc.mipLevels);
			st.res = tex;
			st.curW = desc.width;
			st.curH = desc.height;
			st.curMips = desc.mipLevels;
			st.hold = a_job.src;
			RememberEdge(st.path, st.CurEdge());
			++g_stats.downs;
		}

		// ---------------- Ergebnisse des Hintergrund-Threads uebernehmen (Main-Thread) ----------------

		void LogFail(const std::string& a_path, const std::string& a_err)
		{
			if (g_failLogged < 30) {
				++g_failLogged;
				logger::info("[TextureStream] nicht verkleinerbar: {} ({})", a_path, a_err);
			}
		}

		void ApplyResults()
		{
			std::vector<Result> results;
			{
				std::scoped_lock lock(g_qLock);
				results.swap(g_results);
			}
			for (auto& res : results) {
				const auto& job = res.job;
				const auto  it = g_tex.find(job.r);
				const bool  valid = it != g_tex.end() && it->second.res == job.expectRes && job.r->texture == job.expectRes;
				if (it != g_tex.end()) {
					it->second.busy = false;  // je Textur laeuft hoechstens ein Auftrag
				}
				if (!job.reload) {
					if (!valid) {
						continue;
					}
					auto& st = it->second;
					const auto& f = res.file;
					if (!res.error.empty()) {
						st.probe = Probe::kBad;
						st.eligible = false;
						++g_stats.probesBad;
						LogFail(st.path, res.error);
						continue;
					}
					// Datei = Original; das D3D-Objekt kann schon verkleinert sein (z. B. Eintrag vergessen)
					std::uint32_t skip = 0;
					while (skip < 16 && (f.width >> skip) > st.curW) {
						++skip;
					}
					const bool match = f.fi == st.fi && (f.width >> skip) == st.curW && std::max(1u, f.height >> skip) == st.curH && f.mips >= skip + 1 && f.mips - skip == st.curMips;
					if (!match) {
						st.probe = Probe::kBad;
						st.eligible = false;
						++g_stats.probesBad;
						LogFail(st.path, std::format("Datei {}x{} {} Mips, im Spiel {}x{} {} Mips", f.width, f.height, f.mips, st.curW, st.curH, st.curMips));
						continue;
					}
					st.fullW = f.width;
					st.fullH = f.height;
					st.fullMips = f.mips;
					st.probe = Probe::kOk;
					continue;
				}
				// Neu geladen
				const bool alive = valid && it->second.hold && it->second.hold->rendererTexture == job.r;
				if (!res.error.empty() || !alive) {
					if (res.tex) {
						res.tex->Release();
					}
					if (res.srv) {
						res.srv->Release();
					}
					if (!res.error.empty()) {
						++g_stats.upFails;
						if (it != g_tex.end()) {
							it->second.eligible = false;
							LogFail(job.path, "Neuladen: " + res.error);
						}
					}
					continue;
				}
				auto& st = it->second;
				Swap(job.r, res.tex, res.srv, ChainBytes(st.fi, st.curW, st.curH, st.curMips));
				st.res = res.tex;
				st.curW = std::max(1u, job.fullW >> job.skip);
				st.curH = std::max(1u, job.fullH >> job.skip);
				st.curMips = job.fullMips - job.skip;
				RememberEdge(st.path, st.Reduced() ? st.CurEdge() : 0);
				++g_stats.ups;
				g_stats.upMs += res.ms;
				g_stats.upMB += res.mb;
			}
			ProcessDeferred();
		}

		// ---------------- Szenen-Durchlauf ----------------

		std::vector<RE::NiPointer<RE::NiAVObject>> g_stack;
		bool                                       g_passActive = false;
		Clock::time_point                          g_passStart{};
		std::uint32_t                              g_passFrames = 0, g_passNodes = 0;
		double                                     g_passMs = 0;
		float                                      g_camX = 0, g_camY = 0, g_camZ = 0;
		float                                      g_pixelsPerUnit = 1000.0f;

		constexpr std::uint32_t kMaxTextures = 128;

		// Texturen eines Materials (SEH: Material wird evtl. gerade umgebaut)
		int SafeGather(RE::BSLightingShaderMaterialBase* a_material, RE::NiSourceTexture** a_out) noexcept
		{
			__try {
				std::uint32_t count = a_material->GetTextures(a_out);
				return static_cast<int>(std::min(count, kMaxTextures));
			} __except (1) {
				return -1;
			}
		}

		void VisitGeometry(RE::BSGeometry* a_geom, float a_forceNeed)
		{
			const auto prop = a_geom->GetGeometryRuntimeData().shaderProperty.get();
			const auto lsp = prop ? netimmerse_cast<RE::BSLightingShaderProperty*>(prop) : nullptr;
			const auto material = lsp ? static_cast<RE::BSLightingShaderMaterialBase*>(lsp->material) : nullptr;
			if (!material) {
				return;
			}
			float need = a_forceNeed;
			if (need <= 0) {
				const auto& b = a_geom->worldBound;
				const float dx = b.center.x - g_camX, dy = b.center.y - g_camY, dz = b.center.z - g_camZ;
				const float dist = std::sqrt(dx * dx + dy * dy + dz * dz) - b.radius;
				need = dist <= 1.0f ? 1.0e6f : 2.0f * b.radius / dist * g_pixelsPerUnit;
			}
			RE::NiSourceTexture* textures[kMaxTextures]{};
			const int            n = SafeGather(material, textures);
			for (int i = 0; i < n; ++i) {
				if (textures[i]) {
					OnSeen(textures[i], need);
				}
			}
		}

		void Visit(RE::NiAVObject* a_obj, std::vector<RE::NiPointer<RE::NiAVObject>>& a_stack, float a_forceNeed)
		{
			if (const auto node = a_obj->AsNode()) {
				for (const auto& child : node->GetChildren()) {
					if (child) {
						a_stack.push_back(child);
					}
				}
			} else if (const auto geom = a_obj->AsGeometry()) {
				VisitGeometry(geom, a_forceNeed);
			}
		}

		void UpdateCamera()
		{
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
			g_pixelsPerUnit = screenH / (2.0f * tanHalf);
		}

		void PassEnd()
		{
			const auto now = Clock::now();
			const bool active = Active();
			for (auto it = g_tex.begin(); it != g_tex.end();) {
				auto&      st = it->second;
				const auto r = it->first;
				const bool seen = st.passId == g_passId;
				if (st.eligible && !st.busy) {
					if (st.Reduced() && st.hold && st.probe == Probe::kOk && (!active || (seen && WantedEdge(st, st.passNeed) > st.CurEdge()))) {
						QueueReload(st, r, active ? std::min(st.FullEdge(), WantedEdge(st, st.passNeed) * 2) : st.FullEdge());
					} else if (active && seen) {
						const auto want = WantedEdge(st, st.passNeed);
						if (want * 2 <= st.CurEdge()) {
							if (st.probe == Probe::kNone) {
								QueueProbe(st, r);
							} else if (st.probe == Probe::kOk && st.hold) {
								st.lowTarget = st.lowPasses == 0 ? want : std::max(st.lowTarget, want);
								if (++st.lowPasses >= 2) {
									st.lowPasses = 0;
									st.busy = true;
									g_down.push_back({ r, st.hold, st.lowTarget });
								}
							}
						} else {
							st.lowPasses = 0;
						}
					}
				}
				// Halten nur, solange verkleinert (bis 30 s ungesehen) oder in Bearbeitung
				if (st.hold && !st.busy && (!st.Reduced() || now - st.lastSeen > 30s)) {
					st.hold.reset();
				}
				if (!st.hold && !st.busy && now - st.lastSeen > 120s) {
					it = g_tex.erase(it);
				} else {
					++it;
				}
			}
		}

		// ---------------- Inventar-/Handels-Vorschau: gezeigte Modelle sofort in voller Groesse ----------------

		void PreviewTick()
		{
			ApplyResults();
			const auto ui = RE::UI::GetSingleton();
			if (!ui) {
				return;
			}
			static constexpr std::array kPreviewMenus{ RE::InventoryMenu::MENU_NAME, RE::ContainerMenu::MENU_NAME, RE::BarterMenu::MENU_NAME,
				RE::GiftMenu::MENU_NAME, RE::CraftingMenu::MENU_NAME, RE::MagicMenu::MENU_NAME };
			if (std::ranges::none_of(kPreviewMenus, [&](const auto& a_name) { return ui->IsMenuOpen(a_name); })) {
				return;
			}
			const auto mgr = RE::Inventory3DManager::GetSingleton();
			if (!mgr) {
				return;
			}
			std::vector<RE::NiPointer<RE::NiAVObject>> stack;
			for (const auto& model : mgr->GetRuntimeData().loadedModels) {
				if (model.spModel) {
					stack.push_back(model.spModel);
				}
			}
			std::uint32_t guard = 0;
			while (!stack.empty() && ++guard < 20000) {
				const auto obj = std::move(stack.back());
				stack.pop_back();
				if (obj) {
					Visit(obj.get(), stack, 1.0e6f);
				}
			}
		}

		void Report()
		{
			std::uint32_t managed = 0, eligible = 0, reduced = 0, probeBad = 0;
			double        fullMB = 0, curMB = 0;
			for (const auto& [r, st] : g_tex) {
				++managed;
				if (st.eligible) {
					++eligible;
				}
				if (st.probe == Probe::kBad) {
					++probeBad;
				}
				if (st.Reduced() && st.hold) {
					++reduced;
					fullMB += ChainBytes(st.fi, st.fullW, st.fullH, st.fullMips) / 1048576.0;
					curMB += ChainBytes(st.fi, st.curW, st.curH, st.curMips) / 1048576.0;
				}
			}
			std::size_t queued = 0;
			{
				std::scoped_lock lock(g_qLock);
				queued = g_jobs.size();
			}
			const auto& s = g_stats;
			logger::info("[TextureStream] {} | Texturen {} (verkleinerbar {}, Datei passt nicht {}) | verkleinert {} -> {:.0f} MB statt {:.0f} MB = {:.0f} MB gespart",
				Active() ? "AN" : "AUS", managed, eligible, probeBad, reduced, curMB, fullMB, fullMB - curMB);
			logger::info("[TextureStream]   10 s: verkleinert {} | neu geladen {} ({:.0f} MB, avg {:.0f} ms) | Ladefehler {} | Warteschlange {} | Durchlaeufe {} (avg {:.0f} Frames, {:.0f} Knoten, {:.2f} ms gesamt)",
				s.downs, s.ups, s.upMB, s.ups ? s.upMs / s.ups : 0.0, s.upFails, queued, s.passes, s.passes ? double(s.passFrames) / s.passes : 0.0,
				s.passes ? double(s.passNodes) / s.passes : 0.0, s.passes ? s.passMs / s.passes : 0.0);
			std::size_t remembered = 0;
			{
				std::scoped_lock lock(g_sizeLock);
				remembered = g_loadEdge.size();
			}
			logger::info("[TextureStream]   Stufe 3: {} | gleich verkleinert geladen {} | gemerkte Groessen {}", Config::textureStream.loadReduced ? "AN" : "AUS",
				g_loadedReduced.exchange(0), remembered);
			g_stats = {};
		}
	}

	void Install()
	{
		LoadSizes();
		auto&                           trampoline = SKSE::GetTrampoline();
		REL::Relocation<std::uintptr_t> createSite{ REL::ID(108531), 0x44 };
		REL::Relocation<std::uintptr_t> loadSite{ REL::ID(77301), 0x62 };
		if (*reinterpret_cast<const std::uint8_t*>(createSite.address()) != 0xE8 || *reinterpret_cast<const std::uint8_t*>(loadSite.address()) != 0xE8) {
			logger::warn("TextureStream: Lade-Aufrufe nicht gefunden (andere Spielversion/Mod?) - Stufe 3 inaktiv");
			return;
		}
		CreateRendererTexture::func = trampoline.write_call<5>(createSite.address(), CreateRendererTexture::thunk);
		LoadDDS::func = trampoline.write_call<5>(loadSite.address(), LoadDDS::thunk);
		logger::info("Hook installiert: Texturladen (ID 108531+0x44, ID 77301+0x62) - gleich verkleinert laden");
	}

	void OnFrame()
	{
		static Clock::time_point lastSave = Clock::now();
		if (Clock::now() - lastSave >= 60s) {
			lastSave = Clock::now();
			SaveSizes();
		}
		static Clock::time_point lastReport = Clock::now();
		const auto&              cfg = Config::textureStream;
		const auto               now = Clock::now();

		if (!g_device) {
			if (const auto renderer = RE::BSGraphics::Renderer::GetSingleton()) {
				g_device = renderer->GetRuntimeData().forwarder;
				g_context = renderer->GetRuntimeData().context;
			}
			if (!g_device || !g_context) {
				return;
			}
		}

		ApplyResults();

		if (cfg.analysis && now - lastReport >= 10s) {
			lastReport = now;
			Report();
		}

		const auto ui = RE::UI::GetSingleton();
		if (!ui || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
			// Ladebildschirm: Szene wird umgebaut - Durchlauf verwerfen
			g_stack.clear();
			g_passActive = false;
			for (const auto& job : g_down) {
				if (const auto it = g_tex.find(job.r); it != g_tex.end()) {
					it->second.busy = false;
				}
			}
			g_down.clear();
			return;
		}

		// Verkleinern nach Datenmenge: hoechstens ~64 MB frei werdender Speicher pro Frame (mind. eine Textur),
		// und nicht, solange noch viel alter Speicher auf Freigabe wartet
		std::uint64_t freedThisFrame = 0;
		for (int i = 0; i < 8 && !g_down.empty() && g_deferred.size() < 64; ++i) {
			const auto job = std::move(g_down.front());
			g_down.pop_front();
			std::uint64_t freed = 0;
			Downscale(job, freed);
			freedThisFrame += freed;
			if (freedThisFrame >= kReleaseBytesPerCall) {
				break;
			}
		}

		// Durchlauf in Zeitscheiben
		if (!g_passActive) {
			if (now - g_passStart < 500ms) {
				return;
			}
			const auto root = RE::Main::WorldRootNode();
			if (!root) {
				return;
			}
			g_passActive = true;
			g_passStart = now;
			g_passFrames = g_passNodes = 0;
			g_passMs = 0;
			++g_passId;
			g_stack.clear();
			g_stack.emplace_back(root);
		}
		ZoneScopedN("TextureStream Durchlauf");
		UpdateCamera();
		++g_passFrames;
		const auto t0 = Clock::now();
		const auto budget = std::chrono::duration<double, std::milli>(std::clamp(cfg.budgetMs, 0.05f, 5.0f));
		std::uint32_t n = 0;
		while (!g_stack.empty()) {
			const auto obj = std::move(g_stack.back());
			g_stack.pop_back();
			if (obj) {
				Visit(obj.get(), g_stack, 0.0f);
			}
			++n;
			if ((n & 31) == 0 && Clock::now() - t0 >= budget) {
				break;
			}
		}
		g_passNodes += n;
		g_passMs += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
		if (g_stack.empty()) {
			g_passActive = false;
			++g_stats.passes;
			g_stats.passFrames += g_passFrames;
			g_stats.passNodes += g_passNodes;
			g_stats.passMs += g_passMs;
			PassEnd();
		}
	}
}
