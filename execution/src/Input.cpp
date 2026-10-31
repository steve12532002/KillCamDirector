#include "PCH.h"

#include "Input.h"
#include "Executor.h"
#include "Settings.h"

namespace ED::Input
{
	namespace
	{
		std::atomic<bool> g_capturing{ false };

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
					if (e->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) {
						continue;
					}
					auto dev = e->GetDevice();
					if (dev != RE::INPUT_DEVICE::kKeyboard && dev != RE::INPUT_DEVICE::kMouse) {
						continue;
					}
					auto b = e->AsButtonEvent();
					if (!b || !b->IsDown()) {
						continue;
					}
					// keyboard keys use their scan code, mouse buttons 256 + button (SKSE convention)
					std::uint32_t code = b->GetIDCode() + (dev == RE::INPUT_DEVICE::kMouse ? 256u : 0u);
					if (g_capturing) {
						// left mouse is how you click the capture button, Escape cancels
						if (code == 256u) {
							continue;
						}
						if (code != 0x01) {
							s.executeKey = code;
							s.SaveUser();
						}
						g_capturing = false;
						continue;
					}
					auto ui = RE::UI::GetSingleton();
					if (ui && (ui->GameIsPaused() || ui->IsMenuOpen(RE::Console::MENU_NAME))) {
						continue;
					}
					if (s.enabled && s.executeKey && code == s.executeKey) {
						Executor::Get().RequestExecute();
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void BeginCapture() { g_capturing = true; }
	void CancelCapture() { g_capturing = false; }
	bool IsCapturing() { return g_capturing; }

	const char* KeyName(std::uint32_t a_code)
	{
		static const std::pair<std::uint32_t, const char*> names[] = {
			{ 0x01, "Esc" }, { 0x02, "1" }, { 0x03, "2" }, { 0x04, "3" }, { 0x05, "4" }, { 0x06, "5" }, { 0x07, "6" }, { 0x08, "7" },
			{ 0x09, "8" }, { 0x0A, "9" }, { 0x0B, "0" }, { 0x0C, "-" }, { 0x0D, "=" }, { 0x0E, "Backspace" }, { 0x0F, "Tab" },
			{ 0x10, "Q" }, { 0x11, "W" }, { 0x12, "E" }, { 0x13, "R" }, { 0x14, "T" }, { 0x15, "Y" }, { 0x16, "U" }, { 0x17, "I" },
			{ 0x18, "O" }, { 0x19, "P" }, { 0x1A, "[" }, { 0x1B, "]" }, { 0x1C, "Enter" }, { 0x1D, "Left Ctrl" }, { 0x1E, "A" },
			{ 0x1F, "S" }, { 0x20, "D" }, { 0x21, "F" }, { 0x22, "G" }, { 0x23, "H" }, { 0x24, "J" }, { 0x25, "K" }, { 0x26, "L" },
			{ 0x27, ";" }, { 0x28, "'" }, { 0x29, "`" }, { 0x2A, "Left Shift" }, { 0x2B, "\\" }, { 0x2C, "Z" }, { 0x2D, "X" },
			{ 0x2E, "C" }, { 0x2F, "V" }, { 0x30, "B" }, { 0x31, "N" }, { 0x32, "M" }, { 0x33, "," }, { 0x34, "." }, { 0x35, "/" },
			{ 0x36, "Right Shift" }, { 0x37, "Num *" }, { 0x38, "Left Alt" }, { 0x39, "Space" }, { 0x3A, "Caps Lock" },
			{ 0x3B, "F1" }, { 0x3C, "F2" }, { 0x3D, "F3" }, { 0x3E, "F4" }, { 0x3F, "F5" }, { 0x40, "F6" }, { 0x41, "F7" },
			{ 0x42, "F8" }, { 0x43, "F9" }, { 0x44, "F10" }, { 0x45, "Num Lock" }, { 0x46, "Scroll Lock" }, { 0x47, "Num 7" },
			{ 0x48, "Num 8" }, { 0x49, "Num 9" }, { 0x4A, "Num -" }, { 0x4B, "Num 4" }, { 0x4C, "Num 5" }, { 0x4D, "Num 6" },
			{ 0x4E, "Num +" }, { 0x4F, "Num 1" }, { 0x50, "Num 2" }, { 0x51, "Num 3" }, { 0x52, "Num 0" }, { 0x53, "Num ." },
			{ 0x57, "F11" }, { 0x58, "F12" }, { 0x9C, "Num Enter" }, { 0x9D, "Right Ctrl" }, { 0xB5, "Num /" }, { 0xB8, "Right Alt" },
			{ 0xC7, "Home" }, { 0xC8, "Up" }, { 0xC9, "Page Up" }, { 0xCB, "Left" }, { 0xCD, "Right" }, { 0xCF, "End" },
			{ 0xD0, "Down" }, { 0xD1, "Page Down" }, { 0xD2, "Insert" }, { 0xD3, "Delete" },
			{ 256, "Mouse Left" }, { 257, "Mouse Right" }, { 258, "Mouse Middle" }, { 259, "Mouse 4" }, { 260, "Mouse 5" },
			{ 261, "Mouse 6" }, { 262, "Mouse 7" }, { 263, "Mouse 8" }, { 264, "Wheel Up" }, { 265, "Wheel Down" },
		};
		for (auto& [c, n] : names) {
			if (c == a_code) {
				return n;
			}
		}
		return a_code ? "?" : "None";
	}

	void Install()
	{
		if (auto mgr = RE::BSInputDeviceManager::GetSingleton()) {
			mgr->AddEventSink(Sink::Get());
		}
	}
}
