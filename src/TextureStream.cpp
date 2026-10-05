#include "TextureStream.h"

#include "Config.h"
#include "DetourHelper.h"
#include "GpuMemory.h"

#include <condition_variable>
#include <deque>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

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
			Clock::time_point                  lastReload{};  // zuletzt groesser geladen (Abkuehlzeit gegen Hin und Her)

			std::uint32_t FullEdge() const noexcept { return std::max(fullW, fullH); }
			std::uint32_t CurEdge() const noexcept { return std::max(curW, curH); }
			bool          Reduced() const noexcept { return CurEdge() < FullEdge(); }
		};

		std::unordered_map<RE::BSGraphics::Texture*, TexState> g_tex;

		W::ID3D11Device*        g_device = nullptr;
		W::ID3D11DeviceContext* g_context = nullptr;

		// ---------------- VRAM-Belegung (Budget-Modus) ----------------
		W::IDXGIAdapter3*          g_adapter = nullptr;
		std::atomic<std::uint64_t> g_vramUsage{ 0 }, g_vramBudget{ 0 };
		std::atomic<float>         g_vramPct{ -1.0f };  // -1 = unbekannt
		std::uint32_t              g_reducedCount = 0;   // verkleinerte Texturen (Stand letzter Durchlauf)
		// Windows-Zaehler fuer den ganzen Prozess (Hintergrund-Thread, 1x/s): enthaelt auch CS/DLSS/FG-Speicher
		std::atomic<std::uint64_t> g_procDedicated{ 0 }, g_procShared{ 0 };
		std::atomic<std::uint64_t> g_dxgiUsage{ 0 };
		constexpr std::uint64_t    kSharedPressure = 128ull << 20;  // so viel ausgelagert = VRAM laeuft ueber
		// Auffuellen erst nach ruhiger Phase: direkt nach dem Laden ist der VRAM kurz leer, waehrend die Szene noch
		// hereinkommt -> 0.20.4 fuellte 4 GB auf und verkleinerte gleich wieder (Hin und Her)
		Clock::time_point          g_lastPressure{};
		Clock::time_point          g_lastLoad{};
		constexpr auto             kRefillCalm = 30s;

		void InitAdapter()
		{
			W::IDXGIDevice* dxgiDevice = nullptr;
			if (g_device->QueryInterface(W::IID_IDXGIDevice, reinterpret_cast<void**>(&dxgiDevice)) < 0 || !dxgiDevice) {
				logger::warn("TextureStream: kein IDXGIDevice - Budget-Modus nicht moeglich, verkleinere immer");
				return;
			}
			W::IDXGIAdapter* adapter = nullptr;
			if (dxgiDevice->GetAdapter(&adapter) >= 0 && adapter) {
				adapter->QueryInterface(W::IID_IDXGIAdapter3, reinterpret_cast<void**>(&g_adapter));
				adapter->Release();
			}
			dxgiDevice->Release();
			if (!g_adapter) {
				logger::warn("TextureStream: kein IDXGIAdapter3 - Budget-Modus nicht moeglich, verkleinere immer");
			}
		}

		void UpdateVram()
		{
			if (!g_adapter) {
				return;
			}
			W::DXGI_QUERY_VIDEO_MEMORY_INFO info{};
			if (g_adapter->QueryVideoMemoryInfo(0, W::DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info) >= 0 && info.budget > 0) {
				// DXGI sieht nur einen Teil (0.20.3: 1,5-2,5 GB zu wenig) -> der groessere Wert zaehlt
				const auto usage = std::max(info.currentUsage, g_procDedicated.load(std::memory_order_relaxed));
				g_dxgiUsage.store(info.currentUsage, std::memory_order_relaxed);
				g_vramUsage.store(usage, std::memory_order_relaxed);
				g_vramBudget.store(info.budget, std::memory_order_relaxed);
				g_vramPct.store(100.0f * static_cast<float>(usage) / static_cast<float>(info.budget), std::memory_order_relaxed);
			}
		}

		// Aus Lade-Threads: Belegung hoechstens alle 100 ms selbst abfragen (im Ladebildschirm laeuft OnFrame nicht)
		std::atomic<std::int64_t> g_lastVramQuery{ 0 };

		void RefreshVramThrottled()
		{
			const auto now = Clock::now().time_since_epoch().count();
			auto       last = g_lastVramQuery.load(std::memory_order_relaxed);
			if (now - last >= std::chrono::duration_cast<Clock::duration>(100ms).count() && g_lastVramQuery.compare_exchange_strong(last, now)) {
				UpdateVram();
			}
		}

		// Darf verkleinert werden? Im Budget-Modus nur, wenn der VRAM knapp wird (unbekannt = ja)
		bool Pressure() noexcept
		{
			const auto& cfg = Config::textureStream;
			if (!cfg.budgetMode) {
				return true;
			}
			const float pct = g_vramPct.load(std::memory_order_relaxed);
			// Ausgelagerter Speicher = der VRAM laeuft schon ueber (genau das verursachte die Ruckler beim Umdrehen)
			// Nur solange der VRAM auch nahe der Schwelle ist - Windows holt Ausgelagertes nicht immer sofort zurueck,
			// sonst wuerde endlos weiter verkleinert
			const bool overflowing = g_procShared.load(std::memory_order_relaxed) >= kSharedPressure && pct >= cfg.budgetStartPct - cfg.refillGapPct;
			return pct < 0.0f || pct >= cfg.budgetStartPct || overflowing;
		}

		// Gegen Hin und Her (Plattenlast): groesser laden erst bei deutlich mehr Bedarf, verkleinern erst nach
		// Abkuehlzeit seit dem letzten Neuladen und nach mehreren Durchlaeufen mit geringem Bedarf
		constexpr float kUpMargin = 1.25f;
		constexpr auto  kCooldown = 30s;
		constexpr int   kLowPasses = 3;

		// ---------------- Statistik ----------------
		struct Counters
		{
			std::uint32_t downs = 0, ups = 0, upFails = 0, probesBad = 0;
			std::uint32_t pingPong = 0;
			std::uint32_t refills = 0;  // zum Auffuellen wieder voll geladen  // verkleinert, obwohl erst vor < 60 s neu geladen
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

		// ---------------- RAM-Puffer fuer neu geladene Texturdaten (nur Hintergrund-Thread + Bericht) ----------------
		// Was einmal von der Platte neu geladen wurde, bleibt (bis fRamCacheMB) im RAM. Wechselt die Textur wieder hin und
		// her, kommt sie beim naechsten Mal ohne Plattenzugriff/Entpacken. Am laengsten nicht gebrauchte fliegen zuerst raus.
		struct CacheEntry
		{
			std::vector<std::byte>           data;  // Mip-Stufen ab 'skip' bis zur kleinsten
			std::uint32_t                    skip = 0;
			std::uint32_t                    fullW = 0, fullH = 0, fullMips = 0;
			FormatInfo                       fi;
			std::list<std::string>::iterator lru;
		};
		std::mutex                                  g_cacheLock;
		std::unordered_map<std::string, CacheEntry> g_cache;
		std::list<std::string>                      g_cacheLru;  // vorne = zuletzt benutzt
		std::uint64_t                               g_cacheBytes = 0;
		std::atomic<std::uint32_t>                  g_cacheHits{ 0 }, g_cacheMisses{ 0 };

		std::unordered_map<std::string, std::uint32_t> g_reloadCount;  // Neuladungen je Pfad in dieser Sitzung (unter g_cacheLock)

		std::uint64_t CacheLimit() noexcept { return static_cast<std::uint64_t>(Config::textureStream.ramCacheMB) << 20; }

		void CacheTrim(std::uint64_t a_limit)
		{
			while (g_cacheBytes > a_limit && !g_cacheLru.empty()) {
				const auto it = g_cache.find(g_cacheLru.back());
				if (it != g_cache.end()) {
					g_cacheBytes -= it->second.data.size();
					g_cache.erase(it);
				}
				g_cacheLru.pop_back();
			}
		}

		// Kopie der Daten ab Stufe a_skip, wenn vorhanden
		bool CacheGet(const std::string& a_path, std::uint32_t a_fullW, std::uint32_t a_fullH, std::uint32_t a_fullMips, FormatInfo a_fi, std::uint32_t a_skip,
			std::vector<std::byte>& a_out)
		{
			std::scoped_lock lock(g_cacheLock);
			const auto       it = g_cache.find(a_path);
			if (it == g_cache.end()) {
				return false;
			}
			auto& e = it->second;
			if (e.fullW != a_fullW || e.fullH != a_fullH || e.fullMips != a_fullMips || !(e.fi == a_fi) || e.skip > a_skip) {
				return false;
			}
			std::uint64_t off = 0;
			for (std::uint32_t i = e.skip; i < a_skip; ++i) {
				off += MipBytes(a_fi, std::max(1u, a_fullW >> i), std::max(1u, a_fullH >> i));
			}
			if (off > e.data.size()) {
				return false;
			}
			a_out.assign(e.data.begin() + static_cast<std::ptrdiff_t>(off), e.data.end());
			g_cacheLru.splice(g_cacheLru.begin(), g_cacheLru, e.lru);
			return true;
		}

		void CachePut(const std::string& a_path, std::vector<std::byte>&& a_data, std::uint32_t a_skip, std::uint32_t a_fullW, std::uint32_t a_fullH,
			std::uint32_t a_fullMips, FormatInfo a_fi)
		{
			const auto limit = CacheLimit();
			if (limit == 0 || a_data.size() > limit / 4) {
				return;
			}
			std::scoped_lock lock(g_cacheLock);
			// Nur Texturen, die in dieser Sitzung schon einmal neu geladen wurden (also hin und her wechseln) -
			// einmalige Ladungen (z. B. Auffuellen) wuerden den Puffer sonst sofort mit Nutzlosem fuellen
			if (++g_reloadCount[a_path] < 2) {
				return;
			}
			if (const auto it = g_cache.find(a_path); it != g_cache.end()) {
				if (it->second.skip <= a_skip) {
					return;  // schon mit mindestens so viel Daten vorhanden
				}
				g_cacheBytes -= it->second.data.size();
				g_cacheLru.erase(it->second.lru);
				g_cache.erase(it);
			}
			g_cacheLru.push_front(a_path);
			auto& e = g_cache[a_path];
			e.data = std::move(a_data);
			e.skip = a_skip;
			e.fullW = a_fullW;
			e.fullH = a_fullH;
			e.fullMips = a_fullMips;
			e.fi = a_fi;
			e.lru = g_cacheLru.begin();
			g_cacheBytes += e.data.size();
			CacheTrim(limit);
		}

		void RunJob(Result& a_res)
		{
			const auto& job = a_res.job;
			const auto  t0 = Clock::now();
			std::vector<std::byte> data;
			bool                   fromCache = false;
			if (job.reload && CacheGet(job.path, job.fullW, job.fullH, job.fullMips, job.fi, job.skip, data)) {
				fromCache = true;
				a_res.file.width = job.fullW;
				a_res.file.height = job.fullH;
				a_res.file.mips = job.fullMips;
				a_res.file.fi = job.fi;
				g_cacheHits.fetch_add(1, std::memory_order_relaxed);
			}
			std::unique_ptr<RE::BSResourceNiBinaryStream> stream;
			if (!fromCache) {
			if (job.reload) {
				g_cacheMisses.fetch_add(1, std::memory_order_relaxed);
			}
			stream = std::make_unique<RE::BSResourceNiBinaryStream>(job.path);
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
			}  // !fromCache (Kopf + Springen)
			const auto&         f = a_res.file;
			const std::uint32_t w = std::max(1u, f.width >> job.skip), h = std::max(1u, f.height >> job.skip);
			const std::uint32_t mips = f.mips - job.skip;
			const auto          bytes = ChainBytes(f.fi, w, h, mips);
			if (fromCache && data.size() < bytes) {
				a_res.error = "RAM-Puffer unvollstaendig";
				return;
			}
			if (!fromCache) {
			data.resize(bytes);
			for (std::uint64_t done = 0; done < bytes;) {
				const auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(bytes - done, 16u << 20));
				if (!stream->read(data.data() + done, n)) {
					a_res.error = "Datei zu kurz";
					return;
				}
				done += n;
			}
			stream.reset();
			}  // !fromCache (Daten)

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
			if (!fromCache) {
				CachePut(job.path, std::move(data), job.skip, job.fullW, job.fullH, job.fullMips, job.fi);
			}
		}

		void ApplyResults();
		void PreviewTick();

		void Worker()
		{
			auto lastTick = Clock::now();
			auto lastGpu = Clock::now() - 1s;
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
				if (Clock::now() - lastGpu >= 1s) {
					lastGpu = Clock::now();
					unsigned long long dedicated = 0, shared = 0;
					if (GpuMemory::Query(dedicated, shared)) {
						g_procDedicated.store(dedicated, std::memory_order_relaxed);
						g_procShared.store(shared, std::memory_order_relaxed);
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

		void EnsureWorker()
		{
			if (!g_workerStarted.exchange(true)) {
				std::thread(Worker).detach();
			}
		}

		void Enqueue(Job a_job)
		{
			EnsureWorker();
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

		// benoetigte Kantenlaenge fuer eine Bildschirmgroesse, unabhaengig von Schaltern (fuer gemerkte Groessen)
		std::uint32_t NeededEdge(const TexState& a_st, float a_needPx) noexcept
		{
			const auto    full = a_st.FullEdge();
			const float   need = std::max(a_needPx * Config::textureStream.safetyFactor, Config::textureStream.minEdge);
			std::uint32_t p = 4;
			while (p < need && p < full) {
				p <<= 1;
			}
			return std::min(p, full);
		}

		// benoetigte Kantenlaenge fuer eine Bildschirmgroesse (Zweierpotenz, zwischen fMinEdge und Original)
		std::uint32_t WantedEdge(const TexState& a_st, float a_needPx) noexcept
		{
			return Active() ? NeededEdge(a_st, a_needPx) : a_st.FullEdge();
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
		std::unordered_set<std::string>                g_neverReduce;  // Laden mit unserer Groesse schlug fehl -> nie wieder
		thread_local std::string                       t_loadPath;     // Pfad der Textur, fuer die LoadMaxSize einen Wert lieferte
		bool                                           g_sizesDirty = false;
		std::atomic<std::uint32_t>                     g_loadedReduced{ 0 };
		// Diagnose Stufe 3 (je Berichtszeitraum)
		std::atomic<std::uint32_t> g_diagPreset{ 0 }, g_diagPresetValue{ 0 };  // maxsize schon vom Aufrufer gesetzt
		std::atomic<std::uint32_t> g_diagCreate{ 0 }, g_diagDDS{ 0 }, g_diagNoSrc{ 0 }, g_diagMiss{ 0 }, g_diagOff{ 0 };
		std::mutex                 g_missLock;
		std::vector<std::string>   g_missSamples;
		thread_local RE::NiSourceTexture*              t_loadingSrc = nullptr;

		std::filesystem::path SizesFile()
		{
			auto dir = SKSE::log::log_directory();
			return dir ? *dir / "SkyrimPerf_TextureSizes2.txt" : std::filesystem::path{};
		}

		// a_edge 0 = vergessen (wird wieder voll geladen)
		void RememberEdge(const std::string& a_path, std::uint32_t a_edge)
		{
			if (a_path.empty()) {
				return;
			}
			std::scoped_lock lock(g_sizeLock);
			if (a_edge != 0 && g_neverReduce.contains(a_path)) {
				a_edge = 0;
			}
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
			if (!src) {
				g_diagNoSrc.fetch_add(1, std::memory_order_relaxed);
				return 0;
			}
			if (Config::textureStream.budgetMode) {
				RefreshVramThrottled();
			}
			if (!Config::textureStream.loadReduced || !Config::textureStream.enabled || !Config::masterEnabled.load(std::memory_order_relaxed) || !Pressure()) {
				g_diagOff.fetch_add(1, std::memory_order_relaxed);
				return 0;
			}
			try {
				const auto path = NormalizePath(src->name.c_str());
				std::scoped_lock lock(g_sizeLock);
				const auto it = g_loadEdge.find(path);
				if (it == g_loadEdge.end()) {
					g_diagMiss.fetch_add(1, std::memory_order_relaxed);
					std::scoped_lock missLock(g_missLock);
					if (g_missSamples.size() < 5) {
						g_missSamples.push_back(path);
					}
					return 0;
				}
				g_loadedReduced.fetch_add(1, std::memory_order_relaxed);
				t_loadPath = path;
				return it->second;  // beim Lernen schon auf Gueltigkeit geprueft (SafeLoadEdge)
			} catch (...) {
				return 0;
			}
		}

		// ID 70716 (NiSourceTexture anlegen, Dateiname @0x20) ruft bei +0x198 per Renderer-vtable (call [rax+0xD0]) das
		// Anlegen der Renderer-Textur auf. In Vanilla landet das bei ID 108531 -> 77301 -> 77533; mit Mods (gemessen 0.19.5)
		// kann dort eine fremde Funktion sitzen, die 77533 direkt aufruft -> Textur hier merken, maxsize am Anfang von 77533.
		struct CreateRenderData
		{
			static void thunk(void* a_renderer, RE::NiSourceTexture* a_src)
			{
				g_diagCreate.fetch_add(1, std::memory_order_relaxed);
				const auto prev = t_loadingSrc;
				t_loadingSrc = a_src;
				func(a_renderer, a_src);
				t_loadingSrc = prev;
			}
			static inline void (*func)(void*, RE::NiSourceTexture*) = nullptr;
		};

		// ID 77533 = DDS laden (DirectXTK-Variante), a5 = maxsize; Detour am Funktionsanfang (jeder Aufrufer)
		struct LoadDDS
		{
			static std::int32_t thunk(void* a_device, void* a_stream, void** a_out, void* a_header, std::uint64_t a_maxSize, std::uint64_t a_6)
			{
				g_diagDDS.fetch_add(1, std::memory_order_relaxed);
				// Eine andere Mod kann schon eine Obergrenze mitgeben -> der kleinere Wert gilt (verkleinert nur, nie groesser)
				if (a_maxSize != 0) {
					g_diagPreset.fetch_add(1, std::memory_order_relaxed);
					g_diagPresetValue.store(static_cast<std::uint32_t>(std::min<std::uint64_t>(a_maxSize, UINT32_MAX)), std::memory_order_relaxed);
				}
				t_loadPath.clear();
				bool changed = false;
				if (const auto ours = LoadMaxSize(); ours != 0 && (a_maxSize == 0 || ours < a_maxSize)) {
					a_maxSize = ours;
					changed = true;
				}
				const auto result = func(a_device, a_stream, a_out, a_header, a_maxSize, a_6);
				// Sicherheitsnetz: schlaegt das Laden mit unserer Groesse fehl, diese Textur nie wieder verkleinert laden
				// (0.20.5: zwei Ladenschilder mit krummen Massen -> nicht durch 4 teilbar -> ohne Textur)
				if (changed && result < 0 && !t_loadPath.empty()) {
					{
						std::scoped_lock lock(g_sizeLock);
						g_neverReduce.insert(t_loadPath);
						g_loadEdge.erase(t_loadPath);
						g_sizesDirty = true;
					}
					logger::warn("[TextureStream] Laden mit maxsize {} fehlgeschlagen (0x{:X}) - wird nie mehr verkleinert geladen: {}", a_maxSize,
						static_cast<std::uint32_t>(result), t_loadPath);
				}
				return result;
			}
			static inline std::int32_t (*func)(void*, void*, void**, void*, std::uint64_t, std::uint64_t) = nullptr;
		};

		// maxsize fuer den DDS-Lader, der beim Laden garantiert eine gueltige Textur ergibt (0 = voll laden).
		// Bildet DirectXTK nach: oberste Stufen weglassen, solange Breite oder Hoehe > maxsize. Blockformate brauchen
		// durch 4 teilbare Masse der neuen obersten Stufe - bei krummen Originalmassen sonst Ladefehler.
		std::uint32_t SafeLoadEdge(const TexState& a_st, std::uint32_t a_edge) noexcept
		{
			if (a_edge == 0 || a_edge >= a_st.FullEdge() || a_st.fullMips <= 1) {
				return 0;
			}
			std::uint32_t skip = 0;
			while (skip + 1 < a_st.fullMips && (std::max(1u, a_st.fullW >> skip) > a_edge || std::max(1u, a_st.fullH >> skip) > a_edge)) {
				++skip;
			}
			const std::uint32_t w = std::max(1u, a_st.fullW >> skip), h = std::max(1u, a_st.fullH >> skip);
			if (skip == 0 || w > a_edge || h > a_edge || (a_st.fi.bc && (w % 4 != 0 || h % 4 != 0))) {
				return 0;
			}
			return a_edge;
		}

		void QueueReload(TexState& a_st, RE::BSGraphics::Texture* a_r, std::uint32_t a_targetEdge)
		{
			std::uint32_t skip = 0;
			while ((a_st.FullEdge() >> (skip + 1)) >= a_targetEdge && skip + 1 < a_st.fullMips) {
				++skip;
			}
			// Blockformate: neue oberste Stufe muss durch 4 teilbar sein (krumme Originalmasse) - sonst lieber groesser
			while (skip > 0 && a_st.fi.bc && ((std::max(1u, a_st.fullW >> skip) % 4) != 0 || (std::max(1u, a_st.fullH >> skip) % 4) != 0)) {
				--skip;
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
				if (WantedEdge(st, a_needPx / kUpMargin) > st.CurEdge()) {
					QueueReload(st, r, std::min(st.FullEdge(), WantedEdge(st, a_needPx) * 2));
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
			if (st.lastReload.time_since_epoch().count() != 0 && Clock::now() - st.lastReload < 60s) {
				++g_stats.pingPong;
			}
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
				st.lastReload = Clock::now();
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
			const bool pressure = Pressure();
			// Kandidaten erst sammeln: im Budget-Modus die mit der groessten Ersparnis zuerst verkleinern
			struct Candidate
			{
				DownJob       job;
				std::uint64_t saving;
				float         ratio;  // benoetigt / aktuell - kleiner = weiter weg / weniger sichtbar
			};
			std::vector<Candidate> candidates;
			// VRAM wieder auffuellen (Budget-Modus, deutlich unter der Schwelle): verkleinerte, gerade gesehene Texturen
			struct RefillCandidate
			{
				RE::BSGraphics::Texture* r;
				TexState*                st;
				float                    priority;  // benoetigt / aktuell - die am staerksten gebrauchten zuerst
				std::uint64_t            bytes;     // zusaetzlicher VRAM bei voller Groesse
			};
			std::vector<RefillCandidate> refill;
			const auto&                  cfg = Config::textureStream;
			const float                  pct = g_vramPct.load(std::memory_order_relaxed);
			if (pressure) {
				g_lastPressure = now;
			}
			const bool calm = now - g_lastPressure >= kRefillCalm && now - g_lastLoad >= kRefillCalm;
			const bool wantRefill = active && cfg.budgetMode && cfg.refill && calm && pct >= 0.0f && pct < cfg.budgetStartPct - cfg.refillGapPct;
			g_reducedCount = 0;
			for (auto it = g_tex.begin(); it != g_tex.end(); ++it) {
				auto&      st = it->second;
				const auto r = it->first;
				const bool seen = st.passId == g_passId;
				if (st.Reduced()) {
					++g_reducedCount;
				}
				// Benoetigte Groesse immer lernen (Stufe 3 wendet sie beim Laden nur bei Druck an)
				if (seen && st.eligible && (st.probe == Probe::kOk || st.probe == Probe::kNone)) {
					RememberEdge(st.path, SafeLoadEdge(st, NeededEdge(st, st.passNeed)));
				}
				if (wantRefill && seen && st.eligible && !st.busy && st.hold && st.probe == Probe::kOk && st.Reduced()) {
					refill.push_back({ r, &st, st.passNeed / static_cast<float>(st.CurEdge()),
						ChainBytes(st.fi, st.fullW, st.fullH, st.fullMips) - ChainBytes(st.fi, st.curW, st.curH, st.curMips) });
				}
				if (st.eligible && !st.busy) {
					if (st.Reduced() && st.hold && st.probe == Probe::kOk && (!active || (seen && WantedEdge(st, st.passNeed / kUpMargin) > st.CurEdge()))) {
						QueueReload(st, r, active ? std::min(st.FullEdge(), WantedEdge(st, st.passNeed) * 2) : st.FullEdge());
					} else if (active && seen && pressure) {
						const auto want = WantedEdge(st, st.passNeed);
						if (want * 2 <= st.CurEdge() && now - st.lastReload >= kCooldown) {
							if (st.probe == Probe::kNone) {
								QueueProbe(st, r);
							} else if (st.probe == Probe::kOk && st.hold) {
								st.lowTarget = st.lowPasses == 0 ? want : std::max(st.lowTarget, want);
								if (++st.lowPasses >= kLowPasses) {
									st.lowPasses = 0;
									st.busy = true;
									const std::uint32_t edge = st.lowTarget;
									const std::uint32_t w = std::max(1u, st.curW * edge / st.CurEdge()), h = std::max(1u, st.curH * edge / st.CurEdge());
									candidates.push_back({ { r, st.hold, edge }, ChainBytes(st.fi, st.curW, st.curH, st.curMips) - ChainBytes(st.fi, w, h, st.curMips),
										st.passNeed / static_cast<float>(st.CurEdge()) });
								}
							}
						} else {
							st.lowPasses = 0;
						}
					}
				}
			}
			// Auffuellen: so viel, wie bis zur Mitte zwischen Auffuell- und Verkleinerungsschwelle passt. Ohne Tempolimit -
			// Streaming liest nur, das nutzt eine SSD nicht ab; der Hintergrund-Thread arbeitet die Auftraege nacheinander ab.
			if (!refill.empty()) {
				const double budget = static_cast<double>(g_vramBudget.load(std::memory_order_relaxed));
				const double usage = static_cast<double>(g_vramUsage.load(std::memory_order_relaxed));
				double       room = budget * (cfg.budgetStartPct - cfg.refillGapPct / 2.0) / 100.0 - usage;
				std::ranges::sort(refill, [](const RefillCandidate& a, const RefillCandidate& b) { return a.priority > b.priority; });
				for (const auto& c : refill) {
					if (room < static_cast<double>(c.bytes)) {
						break;
					}
					if (!c.st->busy) {
						QueueReload(*c.st, c.r, c.st->FullEdge());
						room -= static_cast<double>(c.bytes);
						++g_stats.refills;
					}
				}
			}
			for (auto it = g_tex.begin(); it != g_tex.end();) {
				auto& st = it->second;
				// Halten nur fuer laufende Auftraege; sonst nur fuer die Dauer eines Durchlaufs (OnSeen setzt es neu).
				// Lange gehaltene Verweise ueberlebten den Abbau der Welt -> Heap-Beschaedigung (0.19.0).
				if (st.hold && !st.busy) {
					st.hold.reset();
				}
				if (!st.hold && !st.busy && now - st.lastSeen > 120s) {
					it = g_tex.erase(it);
				} else {
					++it;
				}
			}
			// Zuerst, was am wenigsten gebraucht wird (weit weg / hinter der Kamera), bei Gleichstand die groessere Ersparnis -
			// gleiche Ersparnis, aber sichtbare Stellen bleiben laenger scharf
			std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) {
				if (std::abs(a.ratio - b.ratio) > 0.01f) {
					return a.ratio < b.ratio;
				}
				return a.saving > b.saving;
			});
			for (auto& c : candidates) {
				g_down.push_back(std::move(c.job));
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


		// ---------------- Diagnose: wer legt Dateitexturen an? (ID3D11Device::CreateTexture2D, vtable-Index 5) ----------------
		// Stufe 3 griff nie (unsere Lade-Hooks liefen 0x) -> echten Ladeweg im Spiel messen: Aufrufkette in SkyrimSE.exe
		// fuer Texturen mit Mip-Kette und Anfangsdaten (= aus Dateien) zaehlen.
		extern "C" __declspec(dllimport) unsigned short __stdcall RtlCaptureStackBackTrace(unsigned long, unsigned long, void**, unsigned long*);

		using CreateTex2DFn = std::int32_t (*)(W::ID3D11Device*, const W::D3D11_TEXTURE2D_DESC*, const W::D3D11_SUBRESOURCE_DATA*, W::ID3D11Texture2D**);
		CreateTex2DFn                                  g_origCreateTex2D = nullptr;
		std::mutex                                     g_chainLock;
		std::unordered_map<std::string, std::uint32_t> g_chains;

		std::int32_t ProbeCreateTex2D(W::ID3D11Device* a_self, const W::D3D11_TEXTURE2D_DESC* a_desc, const W::D3D11_SUBRESOURCE_DATA* a_init, W::ID3D11Texture2D** a_out)
		{
			if (a_desc && a_init && a_desc->mipLevels > 1 && a_desc->width >= 256) {
				void*               frames[32]{};
				const auto          n = RtlCaptureStackBackTrace(1, 32, frames, nullptr);
				const std::uintptr_t base = REL::Module::get().base();
				std::string          key;
				int                  found = 0;
				for (unsigned i = 0; i < n && found < 5; ++i) {
					const auto a = reinterpret_cast<std::uintptr_t>(frames[i]);
					if (a >= base && a - base < 0x4000000) {
						key += std::format("{}{:X}", found ? ">" : "", a - base);
						++found;
					}
				}
				if (key.empty()) {
					key = "(nicht aus SkyrimSE.exe)";
				}
				std::scoped_lock lock(g_chainLock);
				++g_chains[key];
			}
			return g_origCreateTex2D(a_self, a_desc, a_init, a_out);
		}

		void ReportChains()
		{
			std::vector<std::pair<std::string, std::uint32_t>> list;
			{
				std::scoped_lock lock(g_chainLock);
				list.assign(g_chains.begin(), g_chains.end());
				g_chains.clear();
			}
			if (list.empty()) {
				return;
			}
			std::ranges::sort(list, [](const auto& a, const auto& b) { return a.second > b.second; });
			static const REL::Offset2ID offset2id;
			const auto                  name = [&](std::uintptr_t a_rva) {
				auto it = std::upper_bound(offset2id.begin(), offset2id.end(), a_rva, [](std::uintptr_t a_off, const auto& a_map) { return a_off < a_map.offset; });
				if (it == offset2id.begin()) {
					return std::format("0x{:X}", a_rva);
				}
				--it;
				return std::format("{}+0x{:X}", it->id, a_rva - it->offset);
			};
			logger::info("[TextureStream] Ladeweg-Diagnose: Dateitexturen nach Aufrufkette (SkyrimSE.exe, ID+Offset):");
			for (std::size_t i = 0; i < list.size() && i < 8; ++i) {
				std::string chain;
				std::size_t pos = 0;
				const auto& k = list[i].first;
				while (pos < k.size() && k[0] != '(') {
					const auto end = k.find('>', pos);
					const auto rva = std::strtoull(k.substr(pos, end - pos).c_str(), nullptr, 16);
					chain += (chain.empty() ? "" : " <- ") + name(rva);
					if (end == std::string::npos) {
						break;
					}
					pos = end + 1;
				}
				logger::info("[TextureStream]   {:5}x  {}", list[i].second, chain.empty() ? k : chain);
			}
		}

		void ReportCompact()
		{
			std::uint32_t reduced = 0;
			double        savedMB = 0;
			for (const auto& [r, st] : g_tex) {
				if (st.Reduced()) {
					++reduced;
					savedMB += (ChainBytes(st.fi, st.fullW, st.fullH, st.fullMips) - ChainBytes(st.fi, st.curW, st.curH, st.curMips)) / 1048576.0;
				}
			}
			const auto& s = g_stats;
			logger::info("[TextureStream] VRAM {:.1f}/{:.1f} GB ({:.0f} %, ausgelagert {:.0f} MB) | verkleinert {} Texturen, {:.0f} MB gespart | letzte Minute: "
						 "verkleinert {}, neu geladen {} ({:.0f} MB), aufgefuellt {}, gleich verkleinert geladen {}, aus RAM {}, Ladefehler {}",
				g_vramUsage.load() / 1073741824.0, g_vramBudget.load() / 1073741824.0, std::max(0.0f, g_vramPct.load()), g_procShared.load() / 1048576.0, reduced,
				savedMB, s.downs, s.ups, s.upMB, s.refills, g_loadedReduced.exchange(0), g_cacheHits.exchange(0), s.upFails);
			g_cacheMisses.exchange(0);
			g_diagCreate.exchange(0);
			g_diagDDS.exchange(0);
			g_diagNoSrc.exchange(0);
			g_diagOff.exchange(0);
			g_diagMiss.exchange(0);
			g_diagPreset.exchange(0);
			{
				std::scoped_lock missLock(g_missLock);
				g_missSamples.clear();
			}
			g_stats = {};
		}

		void Report()
		{
			ReportChains();
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
				if (st.Reduced()) {
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
			logger::info("[TextureStream]   10 s: verkleinert {} | neu geladen {} ({:.0f} MB, avg {:.0f} ms) | Ladefehler {} | Hin und Her {} | aufgefuellt {} | Warteschlange {} | Durchlaeufe {} (avg {:.0f} Frames, {:.0f} Knoten, {:.2f} ms gesamt)",
				s.downs, s.ups, s.upMB, s.ups ? s.upMs / s.ups : 0.0, s.upFails, s.pingPong, s.refills, queued, s.passes, s.passes ? double(s.passFrames) / s.passes : 0.0,
				s.passes ? double(s.passNodes) / s.passes : 0.0, s.passes ? s.passMs / s.passes : 0.0);
			std::size_t remembered = 0;
			{
				std::scoped_lock lock(g_sizeLock);
				remembered = g_loadEdge.size();
			}
			logger::info("[TextureStream]   VRAM {:.1f} / {:.1f} GB ({:.0f} %, DXGI {:.1f} GB, ausgelagert {:.0f} MB) | Budget-Modus {} ab {:.0f} % -> {}",
				g_vramUsage.load() / 1073741824.0, g_vramBudget.load() / 1073741824.0, std::max(0.0f, g_vramPct.load()), g_dxgiUsage.load() / 1073741824.0,
				g_procShared.load() / 1048576.0, Config::textureStream.budgetMode ? "AN" : "AUS", Config::textureStream.budgetStartPct,
				Pressure() ? "verkleinern" : "genug Platz");
			{
				std::scoped_lock lock(g_cacheLock);
				CacheTrim(CacheLimit());  // Regler im Menue verkleinert
				logger::info("[TextureStream]   RAM-Puffer {:.0f} / {:.0f} MB, {} Texturen | aus RAM {} | von Platte {}", g_cacheBytes / 1048576.0,
					Config::textureStream.ramCacheMB, g_cache.size(), g_cacheHits.exchange(0), g_cacheMisses.exchange(0));
			}
			logger::info("[TextureStream]   Stufe 3: {} | gleich verkleinert geladen {} | gemerkte Groessen {}", Config::textureStream.loadReduced ? "AN" : "AUS",
				g_loadedReduced.exchange(0), remembered);
			if (Config::analysis.load(std::memory_order_relaxed))
			logger::info("[TextureStream]   Stufe 3 Diagnose: Ladeaufrufe {} | DDS {} | ohne Textur {} | aus/kein Druck {} | Pfad unbekannt {} | maxsize von anderer Mod {} (zuletzt {})", g_diagCreate.exchange(0),
				g_diagDDS.exchange(0), g_diagNoSrc.exchange(0), g_diagOff.exchange(0), g_diagMiss.exchange(0), g_diagPreset.exchange(0), g_diagPresetValue.load());
			{
				std::scoped_lock missLock(g_missLock);
				for (const auto& m : g_missSamples) {
					logger::info("[TextureStream]     unbekannt: {}", m);
				}
				g_missSamples.clear();
			}
			g_stats = {};
		}
	}

	void Reset(const char* a_reason)
	{
		std::size_t jobs = 0;
		{
			std::scoped_lock lock(g_qLock);
			jobs = g_jobs.size();
			g_jobs.clear();  // laufende Ergebnisse finden danach keinen Eintrag mehr und geben ihre Texturen frei
		}
		const auto held = std::ranges::count_if(g_tex, [](const auto& a_e) { return static_cast<bool>(a_e.second.hold); });
		g_stack.clear();
		g_passActive = false;
		g_down.clear();
		g_tex.clear();
		g_reducedCount = 0;
		g_lastLoad = Clock::now();
		logger::info("[TextureStream] Reset ({}): {} gehaltene Texturen und Durchlauf freigegeben, {} Auftraege verworfen", a_reason, held, jobs);
	}

	void InstallLate()
	{
		// Renderer-vtable-Eintrag 0xD0 (von ID 70716+0x198 aufgerufen) umbiegen: Objekt = globaler Zeiger, den 70716 bei
		// +0x18B laedt (48 8B 0D disp32 = mov rcx, [rip+disp32]). Nach anderen Mods (kDataLoaded) -> deren Funktion bleibt drin.
		{
			const auto site = REL::Relocation<std::uintptr_t>{ REL::ID(70716), 0x18B }.address();
			const auto* code = reinterpret_cast<const std::uint8_t*>(site);
			const auto* call = reinterpret_cast<const std::uint8_t*>(site + 0xD);
			if (code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x0D && call[0] == 0xFF && call[1] == 0x90 && call[2] == 0xD0) {
				const auto global = site + 7 + *reinterpret_cast<const std::int32_t*>(site + 3);
				const auto object = *reinterpret_cast<void**>(global);
				if (object) {
					auto**        vtbl = *reinterpret_cast<void***>(object);
					std::uint32_t old = 0;
					if (REX::W32::VirtualProtect(&vtbl[0xD0 / 8], sizeof(void*), 0x40, &old)) {
						CreateRenderData::func = reinterpret_cast<decltype(CreateRenderData::func)>(vtbl[0xD0 / 8]);
						vtbl[0xD0 / 8] = reinterpret_cast<void*>(&CreateRenderData::thunk);
						REX::W32::VirtualProtect(&vtbl[0xD0 / 8], sizeof(void*), old, &old);
						logger::info("Hook installiert: Renderer-Textur anlegen (vtable 0xD0, bisher 0x{:X}) - Textur fuer DDS-Lader merken",
							reinterpret_cast<std::uintptr_t>(CreateRenderData::func));
					}
				} else {
					logger::warn("TextureStream: Renderer-Objekt fuer Texturladen noch nicht da - Stufe 3 inaktiv");
				}
			} else {
				logger::warn("TextureStream: Code in ID 70716 nicht wie erwartet - Stufe 3 inaktiv");
			}
		}

		const auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		const auto device = renderer ? renderer->GetRuntimeData().forwarder : nullptr;
		if (!device) {
			logger::warn("TextureStream: kein Device fuer Ladeweg-Diagnose");
			return;
		}
		// VRAM-Abfrage schon vor dem ersten Spielstand bereit (Budget-Modus beim allerersten Laden)
		if (!g_device) {
			g_device = device;
			g_context = renderer->GetRuntimeData().context;
			InitAdapter();
			UpdateVram();
		}
		EnsureWorker();  // misst ab jetzt 1x/s den Grafikspeicher des Prozesses
		// Ladeweg-Diagnose (Aufrufstapel bei jeder Texturerstellung) nur mit [General] bAnalysis - kostet beim Laden Zeit
		if (!Config::analysis.load(std::memory_order_relaxed)) {
			return;
		}
		auto**        vtbl = *reinterpret_cast<void***>(device);
		std::uint32_t old = 0;
		if (REX::W32::VirtualProtect(&vtbl[5], sizeof(void*), 0x40, &old)) {
			g_origCreateTex2D = reinterpret_cast<CreateTex2DFn>(vtbl[5]);
			vtbl[5] = reinterpret_cast<void*>(&ProbeCreateTex2D);
			REX::W32::VirtualProtect(&vtbl[5], sizeof(void*), old, &old);
			logger::info("TextureStream: Ladeweg-Diagnose aktiv (CreateTexture2D)");
		}
	}

	void GetVram(std::uint64_t& a_usage, std::uint64_t& a_budget)
	{
		a_usage = g_vramUsage.load(std::memory_order_relaxed);
		a_budget = g_vramBudget.load(std::memory_order_relaxed);
	}

	void Install()
	{
		LoadSizes();
		LoadDDS::func = reinterpret_cast<decltype(LoadDDS::func)>(REL::Relocation<std::uintptr_t>{ REL::ID(77533) }.address());
		if (const auto err = DetourHelper::Attach(reinterpret_cast<void**>(&LoadDDS::func), reinterpret_cast<void*>(&LoadDDS::thunk)); err != 0) {
			logger::warn("TextureStream: Detours-Fehler {} an ID 77533 - Stufe 3 inaktiv", err);
			return;
		}
		logger::info("Hook installiert: DDS-Lader (Detour ID 77533) - gleich verkleinert laden");
	}

	void OnFrame()
	{
		static Clock::time_point lastSave = Clock::now();
		if (Clock::now() - lastSave >= 300s) {
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
			InitAdapter();
		}
		static Clock::time_point lastVram{};
		if (now - lastVram >= 250ms) {
			lastVram = now;
			UpdateVram();
		}

		ApplyResults();

		if (now - lastReport >= (cfg.analysis ? 10s : 60s)) {
			lastReport = now;
			if (cfg.analysis) {
				Report();
			} else {
				ReportCompact();
			}
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
		if (!Pressure() && !g_down.empty()) {
			for (const auto& job : g_down) {
				if (const auto it = g_tex.find(job.r); it != g_tex.end()) {
					it->second.busy = false;
				}
			}
			g_down.clear();
		}
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
			// Budget-Modus mit genug VRAM und nichts verkleinert: nur alle 5 s (Groessen lernen), sonst alle 0,5 s
			const bool idle = cfg.budgetMode && !Pressure() && g_reducedCount == 0;
			if (now - g_passStart < (idle ? 5000ms : 500ms)) {
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
