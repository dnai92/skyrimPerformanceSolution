#include "Hooks.h"

#include "InstancingAnalysis.h"
#include "ShadowCulling.h"
#include "Stats.h"

namespace Hooks
{
	namespace
	{
		using VM = RE::BSScript::Internal::VirtualMachine;

		template <class T>
		void WriteVfunc(REL::VariantID a_vtable, std::size_t a_index, const char* a_name)
		{
			REL::Relocation<std::uintptr_t> vtbl{ a_vtable };
			T::func = vtbl.write_vfunc(a_index, T::thunk);
			logger::info("Hook installiert: {} (vfunc 0x{:X})", a_name, a_index);
		}

		// IVirtualMachine::Update (04) - Papyrus-Skriptausfuehrung pro Frame
		struct VMUpdate
		{
			static void thunk(VM* a_this, float a_budget)
			{
				ZoneScopedN("Papyrus VM Update");
				Stats::ScopedTimer timer{ Stats::Zone::PapyrusUpdate };
				func(a_this, a_budget);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// IVirtualMachine::UpdateTasklets (05) - latente Native-Calls / Tasklets
		struct VMUpdateTasklets
		{
			static void thunk(VM* a_this, float a_budget)
			{
				ZoneScopedN("Papyrus Tasklets");
				Stats::ScopedTimer timer{ Stats::Zone::PapyrusTasklets };
				func(a_this, a_budget);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// IVirtualMachine::SetOverstressed (06) - VM meldet Skript-Ueberlast ("Stack-Dump"-Vorstufe)
		struct VMSetOverstressed
		{
			static void thunk(VM* a_this, bool a_set)
			{
				if (a_set) {
					Stats::OnOverstressed();
					TracyMessageLC("Papyrus VM overstressed", 0xFF4040);
				}
				func(a_this, a_set);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Actor::Update (AD) auf PlayerCharacter - einmal pro Spiel-Frame -> Frame-Grenze
		struct PlayerUpdate
		{
			static void thunk(RE::PlayerCharacter* a_this, float a_delta)
			{
				FrameMark;
				Stats::OnFrame();
				ShadowCulling::OnFrame();
				InstancingAnalysis::OnFrame();
				ZoneScopedN("Player Update");
				Stats::ScopedTimer timer{ Stats::Zone::PlayerUpdate };
				func(a_this, a_delta);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Actor::Update (AD) auf Character - pro aktivem NPC
		struct CharacterUpdate
		{
			static void thunk(RE::Character* a_this, float a_delta)
			{
				ZoneScopedN("NPC Update");
				if (TracyIsConnected) {
					if (const auto name = a_this->GetDisplayFullName()) {
						ZoneText(name, std::strlen(name));
					}
				}
				Stats::CountNpcUpdate();
				Stats::ScopedTimer timer{ Stats::Zone::NpcUpdate };
				func(a_this, a_delta);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		class EventSink :
			public RE::BSTEventSink<RE::TESCellFullyLoadedEvent>,
			public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			static EventSink* GetSingleton()
			{
				static EventSink singleton;
				return &singleton;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESCellFullyLoadedEvent* a_event, RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*) override
			{
				Stats::OnCellLoaded();
				if (TracyIsConnected && a_event && a_event->cell) {
					const auto msg = std::format("Cell geladen: {}", a_event->cell->GetFormEditorID());
					TracyMessageC(msg.data(), msg.size(), 0x40A0FF);
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (TracyIsConnected && a_event) {
					const auto msg = std::format("Menue {}: {}", a_event->opening ? "auf" : "zu", a_event->menuName.c_str());
					TracyMessage(msg.data(), msg.size());
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void Install()
	{
		const auto& vmVtbl = RE::VTABLE_BSScript__Internal__VirtualMachine[0];
		WriteVfunc<VMUpdate>(vmVtbl, 0x04, "VirtualMachine::Update");
		WriteVfunc<VMUpdateTasklets>(vmVtbl, 0x05, "VirtualMachine::UpdateTasklets");
		WriteVfunc<VMSetOverstressed>(vmVtbl, 0x06, "VirtualMachine::SetOverstressed");

		WriteVfunc<PlayerUpdate>(RE::VTABLE_PlayerCharacter[0], 0xAD, "PlayerCharacter::Update");
		WriteVfunc<CharacterUpdate>(RE::VTABLE_Character[0], 0xAD, "Character::Update");
	}

	void RegisterEvents()
	{
		if (const auto holder = RE::ScriptEventSourceHolder::GetSingleton()) {
			holder->AddEventSink<RE::TESCellFullyLoadedEvent>(EventSink::GetSingleton());
		}
		if (const auto ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(EventSink::GetSingleton());
		}
		logger::info("Event-Sinks registriert");
	}
}
