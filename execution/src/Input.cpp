#include "PCH.h"

#include "Executor.h"
#include "Settings.h"

namespace ED::Input
{
	namespace
	{
		class Sink : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			static Sink* Get()
			{
				static Sink s;
				return &s;
			}

			RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override
			{
				if (!a_event) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto& s = Settings::Get();
				for (auto e = *a_event; e; e = e->next) {
					if (e->GetEventType() != RE::INPUT_EVENT_TYPE::kButton || e->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
						continue;
					}
					auto b = e->AsButtonEvent();
					if (!b || !b->IsDown()) {
						continue;
					}
					auto ui = RE::UI::GetSingleton();
					if (ui && (ui->GameIsPaused() || ui->IsMenuOpen(RE::Console::MENU_NAME))) {
						continue;
					}
					if (s.enabled && s.executeKey && b->GetIDCode() == s.executeKey) {
						Executor::Get().RequestExecute();
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void Install()
	{
		if (auto mgr = RE::BSInputDeviceManager::GetSingleton()) {
			mgr->AddEventSink(Sink::Get());
		}
	}
}
