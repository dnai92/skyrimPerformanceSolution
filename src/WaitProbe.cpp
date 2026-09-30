#include "WaitProbe.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include "Stats.h"

namespace WaitProbe
{
	namespace
	{
		DWORD         g_mainThread = 0;
		std::int64_t  g_qpcToNs100 = 0;  // QPC-Frequenz (Ticks pro Sekunde)

		inline bool OnMain() noexcept { return GetCurrentThreadId() == g_mainThread; }

		inline std::int64_t Now() noexcept
		{
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			return t.QuadPart;
		}

		inline std::int64_t ToNs(std::int64_t a_ticks) noexcept { return a_ticks * 1'000'000'000 / g_qpcToNs100; }

		// Slot 0 = FSMP, 1 = CBPC
		constexpr std::array<Stats::Zone, 2>    kZones{ Stats::Zone::FsmpWaitMain, Stats::Zone::CbpcWaitMain };
		constexpr std::array<Stats::Counter, 2> kCounters{ Stats::Counter::FsmpWaitCalls, Stats::Counter::CbpcWaitCalls };

		template <int Slot>
		struct Probes
		{
			static inline decltype(&SwitchToThread)            origSwitch = nullptr;
			static inline decltype(&Sleep)                     origSleep = nullptr;
			static inline decltype(&WaitForSingleObjectEx)     origWait = nullptr;
			static inline decltype(&SleepConditionVariableSRW) origCond = nullptr;
			static inline decltype(&AcquireSRWLockExclusive)   origSrw = nullptr;
			static inline decltype(&EnterCriticalSection)      origCs = nullptr;

			static void Record(std::int64_t a_start) noexcept
			{
				Stats::Add(kZones[Slot], ToNs(Now() - a_start));
				Stats::Count(kCounters[Slot]);
			}

			static BOOL WINAPI Switch() noexcept
			{
				if (!OnMain()) {
					return origSwitch();
				}
				const auto s = Now();
				const auto r = origSwitch();
				Record(s);
				return r;
			}
			static void WINAPI SleepProbe(DWORD a_ms) noexcept
			{
				if (!OnMain()) {
					return origSleep(a_ms);
				}
				const auto s = Now();
				origSleep(a_ms);
				Record(s);
			}
			static DWORD WINAPI Wait(HANDLE a_h, DWORD a_ms, BOOL a_alertable) noexcept
			{
				if (!OnMain()) {
					return origWait(a_h, a_ms, a_alertable);
				}
				const auto s = Now();
				const auto r = origWait(a_h, a_ms, a_alertable);
				Record(s);
				return r;
			}
			static BOOL WINAPI Cond(PCONDITION_VARIABLE a_cv, PSRWLOCK a_lock, DWORD a_ms, ULONG a_flags) noexcept
			{
				if (!OnMain()) {
					return origCond(a_cv, a_lock, a_ms, a_flags);
				}
				const auto s = Now();
				const auto r = origCond(a_cv, a_lock, a_ms, a_flags);
				Record(s);
				return r;
			}
			static void WINAPI Srw(PSRWLOCK a_lock) noexcept
			{
				if (!OnMain()) {
					return origSrw(a_lock);
				}
				const auto s = Now();
				origSrw(a_lock);
				Record(s);
			}
			static void WINAPI Cs(LPCRITICAL_SECTION a_cs) noexcept
			{
				if (!OnMain()) {
					return origCs(a_cs);
				}
				const auto s = Now();
				origCs(a_cs);
				Record(s);
			}
		};

		bool PatchEntry(ULONG_PTR* a_slot, void* a_new, void** a_orig) noexcept
		{
			DWORD old = 0;
			if (!VirtualProtect(a_slot, sizeof(*a_slot), PAGE_READWRITE, &old)) {
				return false;
			}
			*a_orig = reinterpret_cast<void*>(*a_slot);
			*a_slot = reinterpret_cast<ULONG_PTR>(a_new);
			VirtualProtect(a_slot, sizeof(*a_slot), old, &old);
			return true;
		}

		template <int Slot>
		ModuleResult PatchModule(const wchar_t* a_name) noexcept
		{
			ModuleResult result{};
			const auto   base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(a_name));
			if (!base) {
				return result;
			}
			result.found = true;

			const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
			const auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
			if (!dir.VirtualAddress) {
				return result;
			}

			using P = Probes<Slot>;
			struct Target
			{
				const char* name;
				void*       probe;
				void**      orig;
			};
			const std::array<Target, 6> targets{ {
				{ "SwitchToThread", reinterpret_cast<void*>(&P::Switch), reinterpret_cast<void**>(&P::origSwitch) },
				{ "Sleep", reinterpret_cast<void*>(&P::SleepProbe), reinterpret_cast<void**>(&P::origSleep) },
				{ "WaitForSingleObjectEx", reinterpret_cast<void*>(&P::Wait), reinterpret_cast<void**>(&P::origWait) },
				{ "SleepConditionVariableSRW", reinterpret_cast<void*>(&P::Cond), reinterpret_cast<void**>(&P::origCond) },
				{ "AcquireSRWLockExclusive", reinterpret_cast<void*>(&P::Srw), reinterpret_cast<void**>(&P::origSrw) },
				{ "EnterCriticalSection", reinterpret_cast<void*>(&P::Cs), reinterpret_cast<void**>(&P::origCs) },
			} };

			for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
				if (!imp->OriginalFirstThunk) {
					continue;
				}
				auto names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->OriginalFirstThunk);
				auto iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);
				for (; names->u1.AddressOfData; ++names, ++iat) {
					if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) {
						continue;
					}
					const auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
					for (const auto& t : targets) {
						if (*t.orig == nullptr && std::strcmp(byName->Name, t.name) == 0) {
							if (PatchEntry(reinterpret_cast<ULONG_PTR*>(&iat->u1.Function), t.probe, t.orig)) {
								++result.patched;
							}
						}
					}
				}
			}
			return result;
		}
	}

	void Install(ModuleResult& a_fsmp, ModuleResult& a_cbpc) noexcept
	{
		g_mainThread = GetCurrentThreadId();
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		g_qpcToNs100 = f.QuadPart;

		a_fsmp = PatchModule<0>(L"hdtsmp64.dll");
		a_cbpc = PatchModule<1>(L"cbp.dll");
	}
}
