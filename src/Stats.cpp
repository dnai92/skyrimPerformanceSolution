#include "Stats.h"

#include "Config.h"

namespace Stats
{
	namespace
	{
		using clock = std::chrono::steady_clock;

		constexpr auto        kReportInterval = 10s;
		constexpr double      kMaxFrameMs = 1000.0;  // laengere Luecken = Ladebildschirm/Pause -> verwerfen
		constexpr std::size_t kZoneCount = static_cast<std::size_t>(Zone::kTotal);
		constexpr std::size_t kCounterCount = static_cast<std::size_t>(Counter::kTotal);

		// Laufende Summen des aktuellen Frames (Actor-Updates koennen auf Worker-Threads laufen)
		std::array<std::atomic<std::int64_t>, kZoneCount> g_frameNs{};
		std::array<std::atomic<std::uint32_t>, kCounterCount> g_frameCounters{};
		std::atomic<std::uint32_t>                        g_frameNpcCount{ 0 };
		std::atomic<std::uint32_t>                        g_overstressed{ 0 };
		std::atomic<std::uint32_t>                        g_cellLoads{ 0 };

		// Nur vom Main-Thread (OnFrame) angefasst
		clock::time_point                        g_lastFrame{};
		clock::time_point                        g_windowStart{};
		std::vector<double>                      g_frameMs;
		std::array<std::vector<double>, kZoneCount> g_zoneMs;
		std::array<std::vector<double>, kCounterCount> g_counterValues;
		std::uint64_t                            g_npcUpdatesInWindow{ 0 };
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

			logger::info("---- {} Frames | {:.1f} FPS | Frame avg {:.2f} ms, p99 {:.2f} ms, max {:.2f} ms | NPC-Updates/Frame {:.1f} | VM overstressed {} | Cell-Loads {}",
				frames, fps, frame.avg, frame.p99, frame.max, npcPerFrame, overstressed, cellLoads);

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

			g_frameMs.clear();
			g_npcUpdatesInWindow = 0;
			g_windowStart = a_now;
		}
	}

	void Init()
	{
		if (const auto dir = logger::log_directory()) {
			g_csvPath = *dir / "SkyrimPerf.csv";
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

	void Add(Zone a_zone, std::int64_t a_ns) noexcept
	{
		g_frameNs[static_cast<std::size_t>(a_zone)].fetch_add(a_ns, std::memory_order_relaxed);
	}

	void Count(Counter a_counter) noexcept
	{
		g_frameCounters[static_cast<std::size_t>(a_counter)].fetch_add(1, std::memory_order_relaxed);
	}

	void CountNpcUpdate() noexcept { g_frameNpcCount.fetch_add(1, std::memory_order_relaxed); }
	void OnOverstressed() noexcept { g_overstressed.fetch_add(1, std::memory_order_relaxed); }
	void OnCellLoaded() noexcept { g_cellLoads.fetch_add(1, std::memory_order_relaxed); }

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

		if (frameMs < kMaxFrameMs) {
			try {
				g_frameMs.push_back(frameMs);
				for (std::size_t i = 0; i < kZoneCount; ++i) {
					g_zoneMs[i].push_back(static_cast<double>(g_frameNs[i].load(std::memory_order_relaxed)) / 1'000'000.0);
				}
				for (std::size_t i = 0; i < kCounterCount; ++i) {
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
