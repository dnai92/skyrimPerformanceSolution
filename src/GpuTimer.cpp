#include "GpuTimer.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>

#include <algorithm>
#include <array>

namespace GpuTimer
{
	namespace
	{
		constexpr int kRing = 6;       // Frames, die gleichzeitig unterwegs sein duerfen
		constexpr int kMaxPairs = 48;  // Messpaare je Abschnitt und Frame (Punktlichter: eins pro Schattenlicht)

		struct Slot
		{
			ID3D11Query* disjoint = nullptr;
			ID3D11Query* start = nullptr;
			ID3D11Query* end = nullptr;
			std::array<std::array<ID3D11Query*, kMaxPairs>, kSectionCount> begins{};
			std::array<std::array<ID3D11Query*, kMaxPairs>, kSectionCount> ends{};
			std::array<int, kSectionCount> count{};
			std::array<int, kSectionCount> open{};
			bool pending = false;  // abgeschlossen, Ergebnis steht aus
		};

		std::array<Slot, kRing> g_slots{};
		ID3D11Device*           g_device = nullptr;
		ID3D11DeviceContext*    g_context = nullptr;
		int                     g_cur = -1;  // laufender Frame (-1 = misst nicht)
		DWORD                   g_thread = 0;
		bool                    g_created = false;
		Window                  g_sum;       // Summen (Mittelwerte erst in Take)
		double                  g_sectionSum[kSectionCount]{};
		double                  g_callSum[kSectionCount]{};
		double                  g_frameSum = 0;

		ID3D11Query* Make(D3D11_QUERY a_type)
		{
			D3D11_QUERY_DESC d{ a_type, 0 };
			ID3D11Query*     q = nullptr;
			return g_device->CreateQuery(&d, &q) >= 0 ? q : nullptr;
		}

		bool Create()
		{
			for (auto& s : g_slots) {
				s.disjoint = Make(D3D11_QUERY_TIMESTAMP_DISJOINT);
				s.start = Make(D3D11_QUERY_TIMESTAMP);
				s.end = Make(D3D11_QUERY_TIMESTAMP);
				if (!s.disjoint || !s.start || !s.end) {
					return false;
				}
				for (int i = 0; i < kSectionCount; ++i) {
					for (int j = 0; j < kMaxPairs; ++j) {
						s.begins[i][j] = Make(D3D11_QUERY_TIMESTAMP);
						s.ends[i][j] = Make(D3D11_QUERY_TIMESTAMP);
						if (!s.begins[i][j] || !s.ends[i][j]) {
							return false;
						}
					}
				}
			}
			return true;
		}

		bool Ts(ID3D11Query* a_q, UINT64& a_out)
		{
			return g_context->GetData(a_q, &a_out, sizeof(a_out), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
		}

		// Abgeschlossene Frames einsammeln, deren Ergebnis die GPU schon geliefert hat
		void Collect()
		{
			for (auto& s : g_slots) {
				if (!s.pending) {
					continue;
				}
				D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
				if (g_context->GetData(s.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
					continue;  // noch nicht fertig
				}
				s.pending = false;
				if (dj.Disjoint || dj.Frequency == 0) {
					++g_sum.disjoint;
					continue;
				}
				const double toMs = 1000.0 / static_cast<double>(dj.Frequency);
				UINT64       t0 = 0, t1 = 0;
				if (!Ts(s.start, t0) || !Ts(s.end, t1) || t1 < t0) {
					++g_sum.skipped;
					continue;
				}
				double sections[kSectionCount]{};
				bool   ok = true;
				for (int i = 0; i < kSectionCount && ok; ++i) {
					for (int j = 0; j < s.count[i]; ++j) {
						UINT64 b = 0, e = 0;
						if (!Ts(s.begins[i][j], b) || !Ts(s.ends[i][j], e)) {
							ok = false;
							break;
						}
						if (e > b) {
							sections[i] += static_cast<double>(e - b) * toMs;
						}
					}
				}
				if (!ok) {
					++g_sum.skipped;
					continue;
				}
				const double frame = static_cast<double>(t1 - t0) * toMs;
				++g_sum.frames;
				g_frameSum += frame;
				g_sum.frameMax = std::max(g_sum.frameMax, frame);
				for (int i = 0; i < kSectionCount; ++i) {
					g_sectionSum[i] += sections[i];
					g_callSum[i] += s.count[i];
				}
			}
		}
	}

	void FrameBoundary(void* a_device, void* a_context, bool a_enabled) noexcept
	{
		auto* device = static_cast<ID3D11Device*>(a_device);
		auto* context = static_cast<ID3D11DeviceContext*>(a_context);
		if (!device || !context) {
			g_cur = -1;
			return;
		}
		if (!g_created) {
			if (!a_enabled) {
				return;
			}
			g_device = device;
			g_context = context;
			g_created = true;
			if (!Create()) {
				g_device = nullptr;  // dauerhaft aus
			}
		}
		if (!g_device || device != g_device || context != g_context) {
			g_cur = -1;
			return;
		}
		g_thread = GetCurrentThreadId();
		// laufenden Frame abschliessen
		if (g_cur >= 0) {
			auto& s = g_slots[g_cur];
			for (int i = 0; i < kSectionCount; ++i) {
				if (s.open[i] >= 0) {  // nicht geschlossener Abschnitt -> verwerfen
					s.count[i] = s.open[i];
					s.open[i] = -1;
				}
			}
			g_context->End(s.end);
			g_context->End(s.disjoint);
			s.pending = true;
			g_cur = -1;
		}
		Collect();
		if (!a_enabled) {
			return;
		}
		// naechsten Frame beginnen (freien Platz im Ring suchen)
		static int next = 0;
		next = (next + 1) % kRing;
		auto& s = g_slots[next];
		if (s.pending) {
			++g_sum.skipped;  // GPU haengt mehr als kRing Frames hinterher
			return;
		}
		s.count.fill(0);
		s.open.fill(-1);
		g_context->Begin(s.disjoint);
		g_context->End(s.start);
		g_cur = next;
	}

	void Begin(Section a_section) noexcept
	{
		if (g_cur < 0 || GetCurrentThreadId() != g_thread) {
			return;
		}
		auto& s = g_slots[g_cur];
		const int n = s.count[a_section];
		if (s.open[a_section] >= 0 || n >= kMaxPairs) {
			return;
		}
		g_context->End(s.begins[a_section][n]);
		s.open[a_section] = n;
	}

	void End(Section a_section) noexcept
	{
		if (g_cur < 0 || GetCurrentThreadId() != g_thread) {
			return;
		}
		auto& s = g_slots[g_cur];
		const int n = s.open[a_section];
		if (n < 0) {
			return;
		}
		g_context->End(s.ends[a_section][n]);
		s.open[a_section] = -1;
		s.count[a_section] = n + 1;
	}

	Window Take() noexcept
	{
		Window w = g_sum;
		if (w.frames > 0) {
			const double f = static_cast<double>(w.frames);
			w.frameAvg = g_frameSum / f;
			for (int i = 0; i < kSectionCount; ++i) {
				w.sectionAvg[i] = g_sectionSum[i] / f;
				w.callsPerFrame[i] = g_callSum[i] / f;
			}
		}
		g_sum = {};
		g_frameSum = 0;
		for (int i = 0; i < kSectionCount; ++i) {
			g_sectionSum[i] = 0;
			g_callSum[i] = 0;
		}
		return w;
	}
}
