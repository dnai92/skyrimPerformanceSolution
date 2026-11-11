#pragma once

// Render-Sperre der Engine (CriticalSection in BSGraphics::Renderer, AE +0x27F0) fuer Arbeit am Immediate Context
// ausserhalb der Engine-Draws. Der Kontext ist nicht threadsicher: RaceMenu, MuDynamicNormalMap u. a. benutzen ihn aus
// Worker-Threads unter genau dieser Sperre (Nexus-Bug StarryCrown, 1.0.74). Rekursiv, ohne Konkurrenz praktisch kostenlos.
// Sperr-Reihenfolge: eigene Sperren (z. B. TextureStream g_stateLock) duerfen gehalten werden - Worker-Threads, die die
// Render-Sperre halten, nehmen keine SPS-Sperren.
class RenderLock
{
public:
	RenderLock() noexcept :
		_renderer(RE::BSGraphics::Renderer::GetSingleton())
	{
		if (_renderer) {
			_renderer->Lock();
		}
	}
	~RenderLock() noexcept
	{
		if (_renderer) {
			_renderer->Unlock();
		}
	}
	RenderLock(const RenderLock&) = delete;
	RenderLock& operator=(const RenderLock&) = delete;

private:
	RE::BSGraphics::Renderer* _renderer;
};
