#pragma once

#include <atomic>
#include <type_traits>

// Vtable-Hook ohne Wettlauf. REL::Relocation::write_vfunc schreibt den neuen Eintrag und liefert das Original erst danach -
// bei "func = vtbl.write_vfunc(...)" wird func also NACH dem Umschreiben gesetzt. Ruft ein anderer Thread die Funktion genau
// dazwischen auf, springt unser Hook ueber einen leeren func-Zeiger ins Leere. Hauptmenue-Szene und Ladebildschirm rendern in
// eigenen Threads, waehrend kDataLoaded laeuft (Nexus-CTD 1.0.63 direkt vor dem Hauptmenue, RIP = 0 in
// LightingRenderPasses::thunk). Deshalb: erst das Original merken, dann den Eintrag umschreiben.
namespace VfuncHook
{
	template <class Func, class Thunk>
	void Write(const REL::Relocation<std::uintptr_t>& a_vtbl, std::size_t a_index, Thunk a_thunk, Func& a_func)
	{
		const auto slot = a_vtbl.address() + a_index * sizeof(std::uintptr_t);
		const auto original = *reinterpret_cast<const volatile std::uintptr_t*>(slot);
		if constexpr (std::is_pointer_v<Func>) {
			a_func = reinterpret_cast<Func>(original);
		} else {
			a_func = original;
		}
		std::atomic_thread_fence(std::memory_order_seq_cst);
		REL::safe_write(slot, reinterpret_cast<std::uintptr_t>(a_thunk));
	}
}
