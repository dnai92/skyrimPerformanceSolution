#include "DetourHelper.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <detours/detours.h>

namespace DetourHelper
{
	long Attach(void** a_target, void* a_detour) noexcept
	{
		if (auto err = DetourTransactionBegin(); err != NO_ERROR) {
			return err;
		}
		DetourUpdateThread(GetCurrentThread());
		if (auto err = DetourAttach(a_target, a_detour); err != NO_ERROR) {
			DetourTransactionAbort();
			return err;
		}
		return DetourTransactionCommit();
	}
}
