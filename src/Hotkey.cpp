#include "Hotkey.h"

#include "Config.h"
#include "Menu.h"

namespace Hotkey
{
	namespace
	{
		class InputSink : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			static InputSink* GetSingleton()
			{
				static InputSink singleton;
				return &singleton;
			}

			RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_events, RE::BSTEventSource<RE::InputEvent*>*) override
			{
				if (!a_events) {
					return RE::BSEventNotifyControl::kContinue;
				}
				for (auto event = *a_events; event; event = event->next) {
					if (event->GetEventType() != RE::INPUT_EVENT_TYPE::kButton || event->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
						continue;
					}
					const auto button = event->AsButtonEvent();
					if (!button || !button->IsDown() || button->GetIDCode() != Config::toggleKey.load(std::memory_order_relaxed)) {
						continue;
					}
					if (const auto ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(RE::Console::MENU_NAME)) {
						continue;
					}
					Toggle();
				}
				return RE::BSEventNotifyControl::kContinue;
			}

		private:
			static void Toggle()
			{
				const bool enabled = !Config::masterEnabled.load(std::memory_order_relaxed);
				Config::masterEnabled.store(enabled, std::memory_order_relaxed);
				logger::info("Hotkey: Culling {}", enabled ? "AN" : "AUS");
				const bool de = Menu::IsGerman();
				RE::SendHUDMessage::ShowHUDMessage(enabled ? (de ? "SkyrimPerf: Optimierungen AN" : "SkyrimPerf: optimizations ON") :
				                                             (de ? "SkyrimPerf: Optimierungen AUS" : "SkyrimPerf: optimizations OFF"));
			}
		};
	}

	void Register()
	{
		if (const auto input = RE::BSInputDeviceManager::GetSingleton()) {
			input->AddEventSink(InputSink::GetSingleton());
			logger::info("Hotkey registriert: Taste 0x{:X} schaltet Culling an/aus", Config::toggleKey.load());
		}
	}
}
