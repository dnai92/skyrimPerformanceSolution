#include "Stats.h"

#include "Config.h"
#include "GpuTimer.h"
#include "LightShadowCache.h"
#include "TextureStream.h"

namespace Stats
{
	namespace
	{
		using clock = std::chrono::steady_clock;

		constexpr auto        kReportInterval = 10s;
		constexpr double      kMaxFrameMs = 1000.0;  // laengere Luecken = Ladebildschirm/Pause -> verwerfen
		// Ruckler-Protokoll (immer an, auch ohne Analyse): jeder Frame ab kHitchMs mit dem, was SPS in diesem und im
		// vorigen Frame getan hat - GPU-Arbeit (Kopien, Freigaben) kann erst im naechsten Frame warten lassen
		constexpr double      kHitchMs = 100.0;
		constexpr std::uint32_t kHitchLinesPerWindow = 20;
		constexpr std::size_t kZoneCount = static_cast<std::size_t>(Zone::kTotal);
		constexpr std::size_t kCounterCount = static_cast<std::size_t>(Counter::kTotal);

		// Laufende Summen des aktuellen Frames (Actor-Updates koennen auf Worker-Threads laufen)
		std::array<std::atomic<std::int64_t>, kZoneCount> g_frameNs{};
		std::array<std::atomic<std::uint32_t>, kCounterCount> g_frameCounters{};
		std::atomic<std::uint32_t>                        g_frameNpcCount{ 0 };
		std::atomic<std::uint32_t>                        g_overstressed{ 0 };
		std::atomic<std::uint32_t>                        g_cellLoads{ 0 };
		std::atomic<bool>                                 g_cellLoadedInFrame{ false };
		std::atomic<bool>                                 g_menuInFrame{ false };

		// Nur vom Main-Thread (OnFrame) angefasst
		clock::time_point                        g_lastFrame{};
		clock::time_point                        g_windowStart{};
		std::vector<double>                      g_frameMs;
		std::array<std::vector<double>, kZoneCount> g_zoneMs;
		std::array<std::vector<double>, kCounterCount> g_counterValues;
		std::uint64_t                            g_npcUpdatesInWindow{ 0 };
		std::uint32_t                            g_hitches{ 0 };
		std::uint32_t                            g_hitchLines{ 0 };
		TextureStream::FrameActivity             g_prevAct;

		std::string Describe(const TextureStream::FrameActivity& a_act)
		{
			if (a_act.downs == 0 && a_act.reloads == 0 && a_act.released == 0) {
				return std::format("nothing ({:.1f} ms{})", a_act.ms, a_act.passEnd ? ", pass evaluated" : "");
			}
			return std::format("downscaled {} ({:.0f} MB), reloaded {} ({:.0f} MB), released {} ({:.0f} MB), {:.1f} ms{}", a_act.downs, a_act.downMB, a_act.reloads,
				a_act.reloadMB, a_act.released, a_act.releasedMB, a_act.ms, a_act.passEnd ? ", pass evaluated" : "");
		}

		void CheckHitch(double a_frameMs)
		{
			// Seit dem letzten Frame-Wechsel = innerhalb dieses Frames (TextureStream::OnFrame laeuft direkt nach Stats::OnFrame)
			const auto act = TextureStream::TakeFrameActivity();
			const auto builds = LightShadowCache::TakeFrameBuilds();
			const bool cell = g_cellLoadedInFrame.exchange(false, std::memory_order_relaxed);
			const bool menu = g_menuInFrame.exchange(false, std::memory_order_relaxed);
			if (a_frameMs >= kHitchMs && a_frameMs < kMaxFrameMs) {
				++g_hitches;
				if (g_hitchLines < kHitchLinesPerWindow) {
					++g_hitchLines;
					// Zeiten der gemessenen Bereiche in diesem Frame (enthalten auch die SPS-Arbeit darin: Culling in den
					// Sonnenschatten, Schatten-Cache in den Fackelschatten, Drosselung in der Licht-Zuordnung)
					const auto z = [](Zone a_z) { return static_cast<double>(g_frameNs[static_cast<std::size_t>(a_z)].load(std::memory_order_relaxed)) / 1e6; };
					const double sun = z(Zone::SunShadowAccumulate) + z(Zone::SunShadowRender), torch = z(Zone::LightShadowRender), lights = z(Zone::LightGather),
								 papyrus = z(Zone::PapyrusUpdate) + z(Zone::PapyrusTasklets), npc = z(Zone::NpcUpdate) + z(Zone::PlayerUpdate), present = z(Zone::PresentWait);
					const auto measured = Enabled() ? std::format(" | measured: sun shadows {:.1f}, torch shadows {:.1f} (cache rebuilds {}), light assignment {:.1f}, papyrus {:.1f}, actors {:.1f}, present wait {:.1f}, not measured {:.1f} ms",
														  sun, torch, builds, lights, papyrus, npc, present, std::max(0.0, a_frameMs - sun - torch - lights - papyrus - npc - present)) :
					                                  std::string(" | times per area only with analysis logging (Debug page)");
					logger::info("[Hitch] {:.0f} ms | SPS this frame: {} | frame before: {} | VRAM {:.0f} %, paged out {:.0f} MB{}{}{}", a_frameMs, Describe(act), Describe(g_prevAct),
						std::max(0.0f, act.vramPct), act.pagedMB, cell ? " | cell loaded" : "", menu ? " | menu opened/closed" : "", measured);
				}
			}
			g_prevAct = act;
		}
		std::filesystem::path                    g_csvPath;

		struct Summary
		{
			double avg{ 0.0 };
			double p99{ 0.0 };
			double max{ 0.0 };
		};

		Summary Summarize(std::vector<double>& a_values)
		{
			if (a_values.empty()) {
				return {};
			}
			Summary s;
			double  sum = 0.0;
			for (const auto v : a_values) {
				sum += v;
			}
			s.avg = sum / static_cast<double>(a_values.size());
			const auto p99Idx = static_cast<std::size_t>(static_cast<double>(a_values.size() - 1) * 0.99);
			std::nth_element(a_values.begin(), a_values.begin() + p99Idx, a_values.end());
			s.p99 = a_values[p99Idx];
			s.max = *std::max_element(a_values.begin(), a_values.end());
			return s;
		}

		void ResetFrameAccumulators() noexcept
		{
			for (auto& ns : g_frameNs) {
				ns.store(0, std::memory_order_relaxed);
			}
			for (auto& c : g_frameCounters) {
				c.store(0, std::memory_order_relaxed);
			}
			g_frameNpcCount.store(0, std::memory_order_relaxed);
		}

		void WriteReport(clock::time_point a_now)
		{
			const auto frames = g_frameMs.size();
			if (frames == 0) {
				return;
			}

			const auto frame = Summarize(g_frameMs);
			const auto fps = frame.avg > 0.0 ? 1000.0 / frame.avg : 0.0;
			const auto overstressed = g_overstressed.exchange(0);
			const auto cellLoads = g_cellLoads.exchange(0);
			const auto npcPerFrame = static_cast<double>(g_npcUpdatesInWindow) / static_cast<double>(frames);

			logger::info("---- {} Frames | {:.1f} FPS | Frame avg {:.2f} ms, p99 {:.2f} ms, max {:.2f} ms | NPC updates/frame {:.1f} | VM overstressed {} | cell loads {} | hitches >= {:.0f} ms: {}",
				frames, fps, frame.avg, frame.p99, frame.max, npcPerFrame, overstressed, cellLoads, kHitchMs, g_hitches);
			g_hitches = 0;
			g_hitchLines = 0;

			// Ohne Analyse-Protokoll nur diese eine Zeile (pro Minute) - Zonen, Zaehler und CSV nur zur Fehlersuche
			if (!Config::analysis.load(std::memory_order_relaxed)) {
				for (auto& v : g_zoneMs) {
					v.clear();
				}
				for (auto& v : g_counterValues) {
					v.clear();
				}
				g_frameMs.clear();
				g_npcUpdatesInWindow = 0;
				g_windowStart = a_now;
				return;
			}

			std::ofstream csv(g_csvPath, std::ios::app);
			const auto    ts = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
			csv << std::format("{},{},{:.2f},{:.3f},{:.3f},{:.3f},{:.1f},{},{}", ts, frames, fps, frame.avg, frame.p99, frame.max, npcPerFrame, overstressed, cellLoads);

			for (std::size_t i = 0; i < kZoneCount; ++i) {
				const auto z = Summarize(g_zoneMs[i]);
				const auto share = frame.avg > 0.0 ? z.avg / frame.avg * 100.0 : 0.0;
				logger::info("     {:<20} avg {:6.2f} ms ({:4.1f}%)  p99 {:6.2f} ms  max {:6.2f} ms", kZoneNames[i], z.avg, share, z.p99, z.max);
				csv << std::format(",{:.3f},{:.3f},{:.3f}", z.avg, z.p99, z.max);
				g_zoneMs[i].clear();
			}
			for (std::size_t i = 0; i < kCounterCount; ++i) {
				const auto c = Summarize(g_counterValues[i]);
				logger::info("     {:<20} avg {:7.0f} /Frame  p99 {:7.0f}  max {:7.0f}", kCounterNames[i], c.avg, c.p99, c.max);
				csv << std::format(",{:.0f},{:.0f},{:.0f}", c.avg, c.p99, c.max);
				g_counterValues[i].clear();
			}
			csv << '\n';

			// GPU-Zeiten (Timestamp-Queries, einige Frames verspaetet): Frame = Frame-Grenze bis Frame-Grenze inkl. Leerlauf
			if (const auto gpu = GpuTimer::Take(); gpu.frames > 0) {
				double      parts = 0;
				std::string sections;
				for (int i = 0; i < GpuTimer::kSectionCount; ++i) {
					parts += gpu.sectionAvg[i];
					sections += std::format(" | {} {:.2f} ms ({:.0f}%, {:.1f}x)", GpuTimer::kSectionNames[i], gpu.sectionAvg[i],
						gpu.frameAvg > 0 ? gpu.sectionAvg[i] / gpu.frameAvg * 100.0 : 0.0, gpu.callsPerFrame[i]);
				}
				logger::info("[GPU] {} frames | frame avg {:.2f} ms, max {:.2f} ms{} | rest {:.2f} ms | skipped {} disjoint {}", gpu.frames, gpu.frameAvg,
					gpu.frameMax, sections, gpu.frameAvg - parts, gpu.skipped, gpu.disjoint);
			}

			g_frameMs.clear();
			g_npcUpdatesInWindow = 0;
			g_windowStart = a_now;
		}
	}

	void Init()
	{
		if (const auto dir = logger::log_directory()) {
			g_csvPath = *dir / "SPS.csv";
		}

		std::ofstream csv(g_csvPath, std::ios::trunc);
		csv << "unix_time,frames,fps,frame_avg_ms,frame_p99_ms,frame_max_ms,npc_updates_per_frame,vm_overstressed,cell_loads";
		for (const auto name : kZoneNames) {
			std::string col{ name };
			std::ranges::replace(col, ' ', '_');
			std::erase(col, ':');
			std::erase(col, '(');
			std::erase(col, ')');
			csv << std::format(",{0}_avg_ms,{0}_p99_ms,{0}_max_ms", col);
		}
		for (const auto name : kCounterNames) {
			std::string col{ name };
			std::ranges::replace(col, ' ', '_');
			std::ranges::replace(col, '-', '_');
			csv << std::format(",{0}_avg,{0}_p99,{0}_max", col);
		}
		csv << '\n';

		for (auto& v : g_counterValues) {
			v.reserve(2048);
		}
		for (auto& v : g_zoneMs) {
			v.reserve(2048);
		}
		g_frameMs.reserve(2048);
	}

	bool Enabled() noexcept { return Config::analysis.load(std::memory_order_relaxed); }

	void Add(Zone a_zone, std::int64_t a_ns) noexcept
	{
		g_frameNs[static_cast<std::size_t>(a_zone)].fetch_add(a_ns, std::memory_order_relaxed);
	}

	void Count(Counter a_counter) noexcept
	{
		// Zaehler erscheinen nur im Analyse-Bericht; ohne ihn keine gemeinsamen Atomics je Mesh aus den Culling-Jobs
		if (!Config::analysis.load(std::memory_order_relaxed)) {
			return;
		}
		g_frameCounters[static_cast<std::size_t>(a_counter)].fetch_add(1, std::memory_order_relaxed);
	}

	void CountNpcUpdate() noexcept { g_frameNpcCount.fetch_add(1, std::memory_order_relaxed); }
	void OnOverstressed() noexcept { g_overstressed.fetch_add(1, std::memory_order_relaxed); }
	void OnCellLoaded() noexcept
	{
		g_cellLoads.fetch_add(1, std::memory_order_relaxed);
		g_cellLoadedInFrame.store(true, std::memory_order_relaxed);
	}
	void OnMenuEvent() noexcept { g_menuInFrame.store(true, std::memory_order_relaxed); }

	namespace
	{
		// Vergleich AN/AUS: bei jedem Umschalten des Hauptschalters eine Zeile fuer den beendeten Abschnitt - auch ohne
		// Analyse-Protokoll, das sonst nur eine Zeile pro Minute schreibt (Wechsel mitten in der Minute nicht trennbar)
		std::vector<double> g_segMs;
		std::uint64_t       g_segNpc = 0;
		bool                g_segOn = true;
		bool                g_segInit = false;

		void TrackSegment(double a_frameMs, bool a_valid) noexcept
		{
			const bool on = Config::masterEnabled.load(std::memory_order_relaxed);
			if (!g_segInit) {
				g_segInit = true;
				g_segOn = on;
			}
			if (on != g_segOn) {
				double total = 0.0;
				for (const auto v : g_segMs) {
					total += v;
				}
				if (total >= 5000.0) {
					const auto s = Summarize(g_segMs);
					logger::info("[Compare] optimizations {} for {:.0f} s: {} frames | {:.1f} FPS | frame avg {:.2f} ms, p99 {:.2f} ms | NPC updates/frame {:.1f}",
						g_segOn ? "ON" : "OFF", total / 1000.0, g_segMs.size(), 1000.0 * static_cast<double>(g_segMs.size()) / total, s.avg, s.p99,
						static_cast<double>(g_segNpc) / static_cast<double>(g_segMs.size()));
				}
				g_segMs.clear();
				g_segNpc = 0;
				g_segOn = on;
			}
			if (a_valid) {
				try {
					g_segMs.push_back(a_frameMs);
				} catch (...) {
				}
				g_segNpc += g_frameNpcCount.load(std::memory_order_relaxed);
			}
		}
	}

	void OnFrame() noexcept
	{
		const auto now = clock::now();
		if (g_lastFrame == clock::time_point{}) {
			g_lastFrame = now;
			g_windowStart = now;
			ResetFrameAccumulators();
			return;
		}

		const auto frameMs = std::chrono::duration<double, std::milli>(now - g_lastFrame).count();
		g_lastFrame = now;
		try {
			CheckHitch(frameMs);
		} catch (...) {
		}

		TrackSegment(frameMs, frameMs < kMaxFrameMs);
		if (frameMs < kMaxFrameMs) {
			try {
				g_frameMs.push_back(frameMs);
				for (std::size_t i = 0; Enabled() && i < kZoneCount; ++i) {
					g_zoneMs[i].push_back(static_cast<double>(g_frameNs[i].load(std::memory_order_relaxed)) / 1'000'000.0);
				}
				for (std::size_t i = 0; Enabled() && i < kCounterCount; ++i) {
					const auto v = g_frameCounters[i].load(std::memory_order_relaxed);
					g_counterValues[i].push_back(static_cast<double>(v));
					TracyPlot(kCounterNames[i], static_cast<std::int64_t>(v));
				}
				g_npcUpdatesInWindow += g_frameNpcCount.load(std::memory_order_relaxed);
			} catch (...) {
			}
		}
		ResetFrameAccumulators();

		if (now - g_windowStart >= (Config::analysis.load(std::memory_order_relaxed) ? kReportInterval : 60s)) {
			try {
				WriteReport(now);
			} catch (...) {
			}
		}
	}
}
