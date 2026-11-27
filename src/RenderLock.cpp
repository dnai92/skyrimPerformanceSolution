#include "RenderLock.h"

#include <Windows.h>

TryRenderLock::TryRenderLock() noexcept :
	_renderer(RE::BSGraphics::Renderer::GetSingleton())
{
	if (_renderer) {
		// GetLock: versionsabhaengiger Platz der CriticalSection (wie Renderer::Lock/Unlock der Engine)
		_owned = ::TryEnterCriticalSection(reinterpret_cast<LPCRITICAL_SECTION>(&_renderer->GetLock())) != FALSE;
	}
}

TryRenderLock::~TryRenderLock() noexcept
{
	if (_owned) {
		::LeaveCriticalSection(reinterpret_cast<LPCRITICAL_SECTION>(&_renderer->GetLock()));
	}
}
