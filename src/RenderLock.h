#pragma once

// Render-Sperre der Engine (CriticalSection in BSGraphics::Renderer, AE ab 1.6.1130 +0x27F0, davor +0x2790) fuer Arbeit
// am Immediate Context ausserhalb der Engine-Draws. Der Kontext ist nicht threadsicher: RaceMenu, MuDynamicNormalMap u. a.
// benutzen ihn aus Worker-Threads unter genau dieser Sperre (Nexus-Bug StarryCrown, 1.0.74).
//
// Seit 1.0.75 nur noch VERSUCHEN (TryRenderLock): die Engine haelt die Sperre z. B. ueber das ganze UI-Zeichnen bis zum
// Present (AE ID 36555) und ihre eigenen Threads warten mitunter auf den Hauptthread. Wartete SPS im Hauptthread darauf,
// konnte das Spiel stehen bleiben (Nexus: Einfrieren beim Aendern der Min./Max. Texturgroesse, 1.0.74). Ist sie belegt,
// wird die Arbeit auf den naechsten Frame verschoben. Rekursiv: haelt der eigene Thread sie schon, gelingt es immer.
class TryRenderLock
{
public:
	TryRenderLock() noexcept;
	~TryRenderLock() noexcept;
	TryRenderLock(const TryRenderLock&) = delete;
	TryRenderLock& operator=(const TryRenderLock&) = delete;

	[[nodiscard]] explicit operator bool() const noexcept { return _owned; }

private:
	RE::BSGraphics::Renderer* _renderer = nullptr;
	bool                      _owned = false;
};
