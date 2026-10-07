#include "Hooks.h"

#include "Config.h"

#include "InstancingAnalysis.h"
#include "ShadowCulling.h"
#include "Stats.h"
#include "TextureStream.h"
#include "WaitProbe.h"

namespace Hooks
{
	namespace
	{
		using VM = RE::BSScript::Internal::VirtualMachine;

		// Alle ~600 Frames: wo wartet der Main-Thread in SkyrimSE.exe? (RVA -> naechstgelegene Address-Library-ID)
		void ReportEngineWaits()
		{
			static std::uint32_t frames = 0;
			if (++frames % 600 != 0 || !Config::analysis.load(std::memory_order_relaxed)) {
				return;
			}
			std::array<WaitProbe::CallerStat, WaitProbe::kMaxCallers> callers{};
			const auto n = WaitProbe::TakeCallers(callers.data(), callers.size());
			std::sort(callers.begin(), callers.begin() + n, [](const auto& a, const auto& b) { return a.ns > b.ns; });

			static const REL::Offset2ID offset2id;
			logger::info("[Engine-Wait] Top wait sites (main thread, over 600 frames):");
			for (std::size_t i = 0; i < n && i < 10; ++i) {
				const auto& c = callers[i];
				auto it = std::upper_bound(offset2id.begin(), offset2id.end(), c.rva, [](std::uintptr_t a_off, const auto& a_map) { return a_off < a_map.offset; });
				std::uint64_t id = 0, delta = 0;
				if (it != offset2id.begin()) {
					--it;
					id = it->id;
					delta = c.rva - it->offset;
				}
				logger::info("[Engine-Wait]   RVA 0x{:X} (ID {} +0x{:X}): {:.2f} ms/frame, {:.1f} calls/frame",
					c.rva, id, delta, c.ns / 1e6 / 600.0, c.calls / 600.0);
			}
		}

		template <class T>
		void WriteVfunc(REL::VariantID a_vtable, std::size_t a_index, const char* a_name)
		{
			REL::Relocation<std::uintptr_t> vtbl{ a_vtable };
			T::func = vtbl.write_vfunc(a_index, T::thunk);
			logger::info("Hook installed: {} (vfunc 0x{:X})", a_name, a_index);
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
#ifdef SPS_VR
				// VR-Build: nur Textur-Streaming (Schatten-/Instancing-Eingriffe fuer VR nicht verifiziert)
				Config::ReloadIfChanged();
				TextureStream::OnFrame();
#else
				ShadowCulling::OnFrame();
				InstancingAnalysis::OnFrame();
				ReportEngineWaits();
#endif
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
				// Vor dem Abbau der Welt/Zellen alle eigenen Verweise freigeben (Ladebildschirm oeffnet vor dem Entladen)
				if (a_event && a_event->opening && (a_event->menuName == RE::MainMenu::MENU_NAME || a_event->menuName == RE::LoadingMenu::MENU_NAME)) {
					TextureStream::Reset(a_event->menuName == RE::MainMenu::MENU_NAME ? "main menu" : "loading screen");
				}
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
#ifdef SPS_VR
		// Skyrim VR 1.4.15: Actor::Update liegt zwei vtable-Plaetze weiter (0xAF statt 0xAD) - offline gegen den
		// Spielcode geprueft (gleicher Code wie SE 1.5.97). Nur der Frame-Takt, keine Statistik-Hooks.
		WriteVfunc<PlayerUpdate>(RE::VTABLE_PlayerCharacter[0], 0xAF, "PlayerCharacter::Update (VR)");
#else
		const auto& vmVtbl = RE::VTABLE_BSScript__Internal__VirtualMachine[0];
		WriteVfunc<VMUpdate>(vmVtbl, 0x04, "VirtualMachine::Update");
		WriteVfunc<VMUpdateTasklets>(vmVtbl, 0x05, "VirtualMachine::UpdateTasklets");
		WriteVfunc<VMSetOverstressed>(vmVtbl, 0x06, "VirtualMachine::SetOverstressed");

		WriteVfunc<PlayerUpdate>(RE::VTABLE_PlayerCharacter[0], 0xAD, "PlayerCharacter::Update");
		WriteVfunc<CharacterUpdate>(RE::VTABLE_Character[0], 0xAD, "Character::Update");
#endif
	}

	void RegisterEvents()
	{
		if (const auto holder = RE::ScriptEventSourceHolder::GetSingleton()) {
			holder->AddEventSink<RE::TESCellFullyLoadedEvent>(EventSink::GetSingleton());
		}
		if (const auto ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(EventSink::GetSingleton());
		}
		logger::info("Event sinks registered");
	}
}
