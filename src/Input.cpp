#include "PCH.h"
#include "Input.h"

#include "Director.h"
#include "Settings.h"

namespace KCD::Input
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
					auto key = b->GetIDCode();
					if (s.toggleKey && key == s.toggleKey) {
						TogglePlugin();
					} else if (s.nextPresetKey && key == s.nextPresetKey) {
						NextPreset();
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void TogglePlugin()
	{
		auto& s = Settings::Get();
		s.enabled = !s.enabled;
		s.SaveUser();
		Director::NotifyText(s.enabled ? "KillCam Director: ON" : "KillCam Director: OFF");
	}

	void NextPreset()
	{
		auto& s = Settings::Get();
		std::vector<std::string> names{ "Random" };
		for (auto& p : s.presets) {
			names.push_back(p.name);
		}
		std::size_t idx = 0;
		for (std::size_t i = 0; i < names.size(); ++i) {
			if (_stricmp(names[i].c_str(), s.preset.c_str()) == 0) {
				idx = i;
			}
		}
		s.preset = names[(idx + 1) % names.size()];
		s.SaveUser();
		auto msg = "KillCam preset: " + s.preset;
		Director::NotifyText(msg.c_str());
	}

	void Install()
	{
		if (auto mgr = RE::BSInputDeviceManager::GetSingleton()) {
			mgr->AddEventSink(Sink::Get());
			logger::info("Input sink installed");
		}
	}
}
