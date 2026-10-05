#include "GpuMemory.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <cwchar>
#include <vector>

#pragma comment(lib, "pdh.lib")

namespace GpuMemory
{
	namespace
	{
		PDH_HQUERY   g_query = nullptr;
		PDH_HCOUNTER g_dedicated = nullptr;
		PDH_HCOUNTER g_shared = nullptr;
		bool         g_failed = false;
		wchar_t      g_prefix[32]{};  // "pid_1234_"

		bool Init() noexcept
		{
			if (g_query || g_failed) {
				return g_query != nullptr;
			}
			swprintf_s(g_prefix, L"pid_%lu_", GetCurrentProcessId());
			if (PdhOpenQueryW(nullptr, 0, &g_query) != ERROR_SUCCESS ||
				PdhAddEnglishCounterW(g_query, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &g_dedicated) != ERROR_SUCCESS ||
				PdhAddEnglishCounterW(g_query, L"\\GPU Process Memory(*)\\Shared Usage", 0, &g_shared) != ERROR_SUCCESS) {
				if (g_query) {
					PdhCloseQuery(g_query);
				}
				g_query = nullptr;
				g_failed = true;
				return false;
			}
			PdhCollectQueryData(g_query);
			return true;
		}

		unsigned long long Sum(PDH_HCOUNTER a_counter) noexcept
		{
			DWORD size = 0, count = 0;
			if (PdhGetFormattedCounterArrayW(a_counter, PDH_FMT_LARGE, &size, &count, nullptr) != PDH_MORE_DATA || size == 0) {
				return 0;
			}
			std::vector<unsigned char> buf(size);
			auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
			if (PdhGetFormattedCounterArrayW(a_counter, PDH_FMT_LARGE, &size, &count, items) != ERROR_SUCCESS) {
				return 0;
			}
			const auto         len = wcslen(g_prefix);
			unsigned long long sum = 0;
			for (DWORD i = 0; i < count; ++i) {
				if (items[i].szName && wcsncmp(items[i].szName, g_prefix, len) == 0 && items[i].FmtValue.CStatus == ERROR_SUCCESS) {
					sum += static_cast<unsigned long long>(items[i].FmtValue.largeValue);
				}
			}
			return sum;
		}
	}

	bool Query(unsigned long long& a_dedicated, unsigned long long& a_shared) noexcept
	{
		if (!Init() || PdhCollectQueryData(g_query) != ERROR_SUCCESS) {
			return false;
		}
		a_dedicated = Sum(g_dedicated);
		a_shared = Sum(g_shared);
		return a_dedicated > 0;
	}
}
