#pragma once

namespace Hooks
{
	// vtable-Hooks: nur Address-Library-IDs aus CommonLib, keine festen Offsets
	void Install();

	// Event-Sinks (Cell geladen, Menue auf/zu) -> Tracy-Messages; erst nach kDataLoaded
	void RegisterEvents();
}
