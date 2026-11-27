#include "GpuMemory.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

		char g_source[160] = "Windows performance counter (GPU Process Memory)";

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

		// ---------------- Linux (Wine / Proton) ----------------
		// Wine's pdh.dll has no "GPU Process Memory" counter, so on Linux the query above always fails and texture
		// streaming is left with the DXGI figure alone (measured on Proton + RADV: ~2.6 GB below what the process
		// really holds, and no overflow signal at all). The game is an ordinary Linux process there, so it can read the
		// kernel's own per-client GPU statistics (Documentation/gpu/drm-usage-stats.rst) for its DRM file descriptors:
		//   drm-resident-vram / drm-memory-vram  -> what the process holds in VRAM right now      (= Dedicated Usage)
		//   amd-evicted-vram (amdgpu only)       -> VRAM buffers the kernel moved to system RAM   (= Shared Usage)
		// Drivers without these keys (NVIDIA proprietary, Intel) report nothing here and behave as before.
		constexpr int kMaxDrmFds = 16;
		wchar_t       g_fdinfoDir[64]{};  // "\\?\unix\proc\<pid>\fdinfo" or "Z:\proc\<pid>\fdinfo"
		unsigned long g_linuxPid = 0;     // the game's Linux process id
		unsigned long g_drmFds[kMaxDrmFds]{};
		int           g_drmFdCount = 0;
		ULONGLONG     g_lastScan = 0;
		ULONGLONG     g_scanMs = 0;
		unsigned      g_scanCount = 0;
		bool          g_scanned = false;
		bool          g_stale = false;  // a remembered descriptor stopped answering - look again

		enum class Backend
		{
			kUnknown,
			kPdh,
			kLinux
		};
		Backend g_backend = Backend::kUnknown;

		bool IsWine() noexcept
		{
			const auto ntdll = GetModuleHandleW(L"ntdll.dll");
			return ntdll && GetProcAddress(ntdll, "wine_get_version");
		}

		// Whole content of a small text file. procfs reports size 0, so read until nothing more comes. 0 = failed.
		DWORD ReadText(const wchar_t* a_path, char* a_buf, DWORD a_cap) noexcept
		{
			const HANDLE file = CreateFileW(a_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				return 0;
			}
			DWORD len = 0, got = 0;
			while (len + 1 < a_cap && ReadFile(file, a_buf + len, a_cap - 1 - len, &got, nullptr) && got > 0) {
				len += got;
			}
			CloseHandle(file);
			a_buf[len] = '\0';
			return len;
		}

		// Value of a line "key:<blanks><number>[ KiB|MiB|GiB]" in bytes; false if no line starts with the key.
		bool Field(const char* a_text, const char* a_key, unsigned long long& a_out) noexcept
		{
			const auto keyLen = std::strlen(a_key);
			for (const char* p = a_text; p && *p;) {
				if (std::strncmp(p, a_key, keyLen) == 0 && p[keyLen] == ':') {
					p += keyLen + 1;
					while (*p == ' ' || *p == '\t') {
						++p;
					}
					if (*p < '0' || *p > '9') {
						return false;
					}
					unsigned long long value = 0;
					for (; *p >= '0' && *p <= '9'; ++p) {
						value = value * 10 + static_cast<unsigned long long>(*p - '0');
					}
					while (*p == ' ' || *p == '\t') {
						++p;
					}
					value <<= *p == 'K' ? 10 : *p == 'M' ? 20 : *p == 'G' ? 30 : 0;
					a_out = value;
					return true;
				}
				p = std::strchr(p, '\n');
				if (p) {
					++p;
				}
			}
			return false;
		}

		struct Client
		{
			unsigned long long id = 0, vram = 0, evicted = 0;
		};

		// false = not a GPU client with VRAM statistics (any other file, or a driver without these keys)
		bool ReadClient(unsigned long a_fd, Client& a_out) noexcept
		{
			wchar_t path[96];
			swprintf_s(path, L"%s\\%lu", g_fdinfoDir, a_fd);
			char text[8192];
			if (ReadText(path, text, sizeof(text)) == 0 || !Field(text, "drm-client-id", a_out.id)) {
				return false;
			}
			if (!Field(text, "drm-resident-vram", a_out.vram) && !Field(text, "drm-memory-vram", a_out.vram)) {
				return false;
			}
			a_out.evicted = 0;
			Field(text, "amd-evicted-vram", a_out.evicted);
			return true;
		}

		// Find the process's DRM descriptors. The game holds thousands of descriptors and each one costs a file open
		// through Wine, so the result is remembered and only the few DRM ones are read once per second.
		void ScanFds() noexcept
		{
			const auto start = GetTickCount64();
			g_drmFdCount = 0;
			wchar_t pattern[96];
			swprintf_s(pattern, L"%s\\*", g_fdinfoDir);
			WIN32_FIND_DATAW entry{};
			const HANDLE     find = FindFirstFileW(pattern, &entry);
			if (find != INVALID_HANDLE_VALUE) {
				do {
					if (entry.cFileName[0] < L'0' || entry.cFileName[0] > L'9') {
						continue;
					}
					const auto fd = std::wcstoul(entry.cFileName, nullptr, 10);
					Client     client;
					if (g_drmFdCount < kMaxDrmFds && ReadClient(fd, client)) {
						g_drmFds[g_drmFdCount++] = fd;
					}
				} while (FindNextFileW(find, &entry));
				FindClose(find);
			}
			g_lastScan = GetTickCount64();
			g_scanMs = g_lastScan - start;
			g_scanned = true;
			g_stale = false;
			++g_scanCount;
			std::snprintf(g_source, sizeof(g_source), "Linux kernel DRM statistics (%ls, %d GPU descriptor%s, search took %llu ms)", g_fdinfoDir, g_drmFdCount,
				g_drmFdCount == 1 ? "" : "s", g_scanMs);
		}

		// Own Linux process id. "/proc/self" cannot be used for reading: Wine lets its server process open every file,
		// so "self" would be the wineserver (seen in the first test build: 0 GPU descriptors). A directory listing is
		// different - Wine looks up each entry from the calling process, so in a listing of /proc the magic link
		// "self" resolves to this process and carries the same file id (inode) as the numeric entry of our pid.
		unsigned long OwnLinuxPid(const wchar_t* a_proc) noexcept
		{
			unsigned long      result = 0;
			unsigned long long selfId = 0;
			for (int pass = 0; pass < 2 && result == 0; ++pass) {
				const HANDLE dir = CreateFileW(a_proc, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
					FILE_FLAG_BACKUP_SEMANTICS, nullptr);
				if (dir == INVALID_HANDLE_VALUE) {
					return 0;
				}
				static char buf[65536];
				bool        first = true;
				while (result == 0 && GetFileInformationByHandleEx(dir, first ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo, buf, sizeof(buf))) {
					first = false;
					for (auto* entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buf);;
						 entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(reinterpret_cast<const char*>(entry) + entry->NextEntryOffset)) {
						const auto len = entry->FileNameLength / sizeof(wchar_t);
						const auto id = static_cast<unsigned long long>(entry->FileId.QuadPart);
						if (pass == 0) {
							if (len == 4 && std::wcsncmp(entry->FileName, L"self", 4) == 0) {
								selfId = id;
							}
						} else if (id == selfId && len > 0 && len < 16 && entry->FileName[0] >= L'0' && entry->FileName[0] <= L'9') {
							wchar_t name[16]{};
							std::wmemcpy(name, entry->FileName, len);
							result = std::wcstoul(name, nullptr, 10);
							break;
						}
						if (entry->NextEntryOffset == 0) {
							break;
						}
					}
				}
				CloseHandle(dir);
				if (pass == 0 && selfId == 0) {
					return 0;
				}
			}
			return result;
		}

		bool LinuxInit() noexcept
		{
			// "\\?\unix\" reaches the host file system without depending on a drive letter; Z: is Wine's default mapping of /
			static constexpr const wchar_t* kProcDirs[] = { L"\\\\?\\unix\\proc", L"Z:\\proc" };
			for (const auto proc : kProcDirs) {
				if (const auto pid = OwnLinuxPid(proc)) {
					g_linuxPid = pid;
					swprintf_s(g_fdinfoDir, L"%s\\%lu\\fdinfo", proc, pid);
					return true;
				}
			}
			std::snprintf(g_source, sizeof(g_source), "Wine detected, but the game's Linux process was not found under /proc");
			return false;
		}

		bool LinuxQuery(unsigned long long& a_dedicated, unsigned long long& a_shared) noexcept
		{
			// A search opens every descriptor of the process (measured: ~0.55 s for 5,400 on this thread), so it is rare:
			// at the first query, once more a minute later (the first one runs in the main menu, before every graphics
			// device exists), then every 30 min, 10 s after a remembered descriptor went away, and once a minute
			// while none has been found yet - but only five times: a driver without these statistics never gets one
			const auto now = GetTickCount64();
			const auto age = now - g_lastScan;
			if (!g_scanned || age > (g_scanCount < 2 ? 60'000u : 1'800'000u) || (g_stale && age > 10'000) ||
				(g_drmFdCount == 0 && g_scanCount < 5 && age > 60'000)) {
				ScanFds();
			}
			// Duplicated descriptors of one DRM file share a client id and must be counted once
			unsigned long long ids[kMaxDrmFds];
			int                idCount = 0;
			unsigned long long vram = 0, evicted = 0;
			for (int i = 0; i < g_drmFdCount; ++i) {
				Client client;
				if (!ReadClient(g_drmFds[i], client)) {
					g_stale = true;
					continue;
				}
				bool seen = false;
				for (int k = 0; k < idCount; ++k) {
					seen = seen || ids[k] == client.id;
				}
				if (seen) {
					continue;
				}
				ids[idCount++] = client.id;
				vram += client.vram;
				evicted += client.evicted;
			}
			a_dedicated = vram;
			a_shared = evicted;
			return vram > 0;
		}
	}

	bool Query(unsigned long long& a_dedicated, unsigned long long& a_shared) noexcept
	{
		if (g_backend == Backend::kUnknown) {
			g_backend = IsWine() && LinuxInit() ? Backend::kLinux : Backend::kPdh;
		}
		if (g_backend == Backend::kLinux) {
			return LinuxQuery(a_dedicated, a_shared);
		}
		if (!Init() || PdhCollectQueryData(g_query) != ERROR_SUCCESS) {
			return false;
		}
		a_dedicated = Sum(g_dedicated);
		a_shared = Sum(g_shared);
		return a_dedicated > 0;
	}

	const char* Source() noexcept { return g_source; }
}
