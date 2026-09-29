// Wertet die CPU-Sampling-Daten einer .tracy-Datei aus:
//   - pro Thread: Anzahl Samples
//   - pro Thread: exklusive/inklusive Samples je Modul (DLL/EXE)
//   - fuer die wichtigsten Threads: Top-Funktionen (exklusiv), bei Modulen ohne Symbole mit Adresse
// Aufruf: tracy-sampling-export <trace.tracy> [topN]

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../extern/tracy/server/TracyFileRead.hpp"
#include "../../extern/tracy/server/TracyWorker.hpp"

namespace
{
	struct Counter
	{
		uint64_t excl = 0;
		uint64_t incl = 0;
	};

	struct ThreadResult
	{
		std::string                               name;
		std::string                               owner;  // haeufigstes Nicht-System-Modul im Callstack
		uint64_t                                  samples = 0;
		uint64_t                                  unresolved = 0;
		uint64_t                                  sleeping = 0;  // Samples mit Sleep/SwitchToThread im Stack
		std::unordered_map<std::string, Counter>  modules;
		std::unordered_map<std::string, Counter>  symbols;
		std::unordered_map<std::string, uint64_t> exeCallers;  // naechster SkyrimSE.exe-Frame fuer Samples in Fremd-Modulen
	};

	std::string BaseName(const char* a_path)
	{
		std::string s = a_path ? a_path : "?";
		const auto  pos = s.find_last_of("\\/");
		s = pos == std::string::npos ? s : s.substr(pos + 1);
		// Tracy liefert Modulnamen als "[name.dll]"
		if (s.size() > 2 && s.front() == '[' && s.back() == ']') {
			s = s.substr(1, s.size() - 2);
		}
		return s;
	}

	bool IsSystemModule(const std::string& a_module)
	{
		static const std::unordered_set<std::string> kSystem{
			"<kernel>", "ntdll.dll", "KERNEL32.DLL", "KERNELBASE.dll", "ucrtbase.dll", "VCRUNTIME140.dll",
			"VCRUNTIME140_1.dll", "MSVCP140.dll", "msvcrt.dll", "USER32.dll", "win32u.dll", "VCOMP140.DLL", "unknown"
		};
		return kSystem.contains(a_module);
	}

	// Nur Samples in Frames mit Dauer in [min, max) ms zaehlen (0/0 = alle)
	struct FrameFilter
	{
		std::vector<std::pair<int64_t, int64_t>> ranges;  // sortiert nach Start

		bool Contains(int64_t a_time) const
		{
			auto it = std::upper_bound(ranges.begin(), ranges.end(), a_time, [](int64_t t, const auto& r) { return t < r.first; });
			if (it == ranges.begin()) return false;
			--it;
			return a_time < it->second;
		}
	};

	std::string FrameLabel(const tracy::Worker& a_worker, const tracy::CallstackFrameData& a_frame, const tracy::CallstackFrameId& a_id)
	{
		const auto  image = BaseName(a_worker.GetString(a_frame.imageName));
		const auto& outer = a_frame.data[a_frame.size - 1];
		std::string name = a_worker.GetString(outer.name);
		// Ohne Symbole liefert Tracy "[unknown]" o.ae. -> Adresse anhaengen, damit Hotspots unterscheidbar bleiben
		if (name.empty() || name[0] == '[' || name.rfind("0x", 0) == 0) {
			char buf[64];
			std::snprintf(buf, sizeof(buf), "@0x%" PRIx64, a_worker.GetCanonicalPointer(a_id));
			name = buf;
		}
		return image + "!" + name;
	}

	template <class Map>
	auto SortedBy(const Map& a_map, auto a_key)
	{
		std::vector<std::pair<std::string, typename Map::mapped_type>> v(a_map.begin(), a_map.end());
		std::sort(v.begin(), v.end(), [&](const auto& a, const auto& b) { return a_key(a.second) > a_key(b.second); });
		return v;
	}
}

int main(int argc, char** argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "Usage: %s <trace.tracy> [topN] [minFrameMs maxFrameMs]\n", argv[0]);
		return 1;
	}
	const std::size_t topN = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 40;
	const double      minFrameMs = argc > 3 ? std::atof(argv[3]) : 0.0;
	const double      maxFrameMs = argc > 4 ? std::atof(argv[4]) : 0.0;

	std::unique_ptr<tracy::FileRead> file(tracy::FileRead::Open(argv[1]));
	if (!file) {
		std::fprintf(stderr, "Kann %s nicht oeffnen\n", argv[1]);
		return 1;
	}
	tracy::Worker worker(*file, tracy::EventType::All, false);

	std::printf("Trace: %s\nDauer: %.1f s | Samples gesamt: %" PRIu64 " | Frames: %zu\n\n",
		argv[1], worker.GetLastTime() / 1e9, worker.GetCallstackSampleCount(), worker.GetFrameCount(*worker.GetFramesBase()));

	FrameFilter filter;
	const bool  useFilter = minFrameMs > 0.0 || maxFrameMs > 0.0;
	if (useFilter) {
		const auto& fd = *worker.GetFramesBase();
		int64_t     filteredNs = 0;
		for (std::size_t i = 0; i < worker.GetFrameCount(fd); ++i) {
			const auto ms = worker.GetFrameTime(fd, i) / 1e6;
			if (ms >= minFrameMs && (maxFrameMs <= 0.0 || ms < maxFrameMs)) {
				filter.ranges.emplace_back(worker.GetFrameBegin(fd, i), worker.GetFrameEnd(fd, i));
				filteredNs += worker.GetFrameTime(fd, i);
			}
		}
		std::printf("Frame-Filter: %.1f-%.1f ms -> %zu Frames, %.1f s\n\n", minFrameMs, maxFrameMs, filter.ranges.size(), filteredNs / 1e9);
	}

	std::vector<ThreadResult> results;
	for (const auto* td : worker.GetThreadData()) {
		if (td->samples.empty()) {
			continue;
		}
		ThreadResult r;
		r.name = worker.GetThreadName(td->id);
		r.name += " [" + std::to_string(td->id) + "]";

		for (const auto& sample : td->samples) {
			const auto cs = sample.callstack.Val();
			if (cs == 0 || (useFilter && !filter.Contains(sample.time.Val()))) {
				continue;
			}
			const auto& stack = worker.GetCallstack(cs);
			r.samples++;

			std::unordered_set<std::string> seenModules;
			std::unordered_set<std::string> seenSymbols;
			std::string                     leafModule;
			bool                            exeCallerFound = false;
			bool                            sleeping = false;

			for (uint16_t i = 0; i < stack.size(); ++i) {
				const auto* frame = worker.GetCallstackFrame(stack[i]);
				if (!frame || frame->size == 0) {
					if (i == 0) {
						r.unresolved++;
					}
					continue;
				}
				const auto module = BaseName(worker.GetString(frame->imageName));
				const auto label = FrameLabel(worker, *frame, stack[i]);
				if (!sleeping && (label.ends_with("!NtDelayExecution") || label.ends_with("!SwitchToThread") || label.ends_with("!NtYieldExecution"))) {
					sleeping = true;
					r.sleeping++;
				}

				if (i == 0) {
					leafModule = module;
					r.modules[module].excl++;
					r.symbols[label].excl++;
				} else if (!exeCallerFound && leafModule != "SkyrimSE.exe" && module == "SkyrimSE.exe") {
					r.exeCallers[label]++;
					exeCallerFound = true;
				}
				if (seenModules.insert(module).second) {
					r.modules[module].incl++;
				}
				if (seenSymbols.insert(label).second) {
					r.symbols[label].incl++;
				}
			}
		}
		uint64_t best = 0;
		for (const auto& [module, c] : r.modules) {
			if (!IsSystemModule(module) && c.incl > best) {
				best = c.incl;
				r.owner = module;
			}
		}
		if (r.owner.empty()) {
			r.owner = "(nur System)";
		}
		if (r.samples > 0) {
			results.push_back(std::move(r));
		}
	}

	std::sort(results.begin(), results.end(), [](const auto& a, const auto& b) { return a.samples > b.samples; });

	uint64_t totalSamples = 0;
	for (const auto& r : results) {
		totalSamples += r.samples;
	}

	std::printf("== Threads nach Samples (Owner = haeufigstes Mod-/Spielmodul im Callstack) ==\n");
	for (const auto& r : results) {
		std::printf("%10" PRIu64 "  %5.1f%%  sleep/spin %5.1f%%  %-28s %s\n", r.samples, r.samples * 100.0 / totalSamples,
			r.sleeping * 100.0 / r.samples, r.owner.c_str(), r.name.c_str());
	}

	struct OwnerSum
	{
		uint64_t threads = 0, samples = 0, sleeping = 0;
	};
	std::unordered_map<std::string, OwnerSum> owners;
	for (const auto& r : results) {
		auto& o = owners[r.owner];
		o.threads++;
		o.samples += r.samples;
		o.sleeping += r.sleeping;
	}
	std::printf("\n== CPU-Zeit nach Thread-Owner ==\n");
	for (const auto& [name, o] : SortedBy(owners, [](const OwnerSum& o) { return o.samples; })) {
		std::printf("  %5.1f%% aller Samples  %3" PRIu64 " Threads  davon sleep/spin %5.1f%%  %s\n", o.samples * 100.0 / totalSamples, o.threads,
			o.sleeping * 100.0 / o.samples, name.c_str());
	}

	const std::size_t detailThreads = std::min<std::size_t>(results.size(), 6);
	for (std::size_t t = 0; t < detailThreads; ++t) {
		const auto& r = results[t];
		const double total = static_cast<double>(std::max<uint64_t>(r.samples, 1));
		std::printf("\n\n######## Thread %s | %" PRIu64 " Samples (unaufgeloest: %" PRIu64 ")\n", r.name.c_str(), r.samples, r.unresolved);

		std::printf("\n-- Module (exkl%% = Code laeuft IN diesem Modul, inkl%% = Modul irgendwo im Callstack)\n");
		for (const auto& [name, c] : SortedBy(r.modules, [](const Counter& c) { return c.incl; })) {
			std::printf("  exkl %5.1f%%  inkl %5.1f%%  %s\n", c.excl * 100.0 / total, c.incl * 100.0 / total, name.c_str());
		}

		std::printf("\n-- Top %zu Funktionen exklusiv\n", topN);
		std::size_t n = 0;
		for (const auto& [name, c] : SortedBy(r.symbols, [](const Counter& c) { return c.excl; })) {
			if (n++ >= topN || c.excl == 0) break;
			std::printf("  exkl %5.1f%%  inkl %5.1f%%  %s\n", c.excl * 100.0 / total, c.incl * 100.0 / total, name.c_str());
		}

		std::printf("\n-- Top %zu Funktionen inklusiv\n", topN);
		n = 0;
		for (const auto& [name, c] : SortedBy(r.symbols, [](const Counter& c) { return c.incl; })) {
			if (n++ >= topN) break;
			std::printf("  inkl %5.1f%%  exkl %5.1f%%  %s\n", c.incl * 100.0 / total, c.excl * 100.0 / total, name.c_str());
		}

		std::printf("\n-- Aufrufer in SkyrimSE.exe fuer Samples in anderen Modulen (Top %zu)\n", topN);
		n = 0;
		for (const auto& [name, cnt] : SortedBy(r.exeCallers, [](uint64_t v) { return v; })) {
			if (n++ >= topN) break;
			std::printf("  %5.1f%%  %s\n", cnt * 100.0 / total, name.c_str());
		}
	}
	return 0;
}
