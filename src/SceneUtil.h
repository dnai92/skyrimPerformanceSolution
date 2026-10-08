#pragma once

namespace SceneUtil
{
	// Referenz, die genau an diesem Knoten haengt (Wurzelknoten einer Referenz), ohne Suche nach oben.
	// NiAVObject::GetUserData (CommonLib) laeuft bei leerem Eintrag rekursiv bis zur Wurzel und fragt auf jeder Ebene die
	// Spielversion ab - in Schleifen ueber die Vorfahren und je Objekt im Culling ~6 % des Haupt-Threads (Tracy, Markt).
	inline RE::TESObjectREFR* OwnUserData(const RE::NiAVObject* a_obj) noexcept
	{
#ifdef SPS_VR
		constexpr std::uintptr_t kOffset = 0x110;
#else
		constexpr std::uintptr_t kOffset = 0x0F8;
#endif
		return *reinterpret_cast<RE::TESObjectREFR* const*>(reinterpret_cast<std::uintptr_t>(a_obj) + kOffset);
	}
}
