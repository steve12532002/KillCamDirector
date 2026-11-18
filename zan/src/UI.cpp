#include "PCH.h"

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <windows.h>

#include "SKSEMenuFramework.h"
#include "Glyph.h"
#include "Input.h"

#include "Core.h"
#include "Settings.h"

namespace Zan::UI
{
	namespace
	{
		// Draws the built-in 斬 bitmap as horizontal runs of rectangles (the menu font has no CJK glyphs)
		void DrawGlyph(ImGuiMCP::ImDrawList* a_dl, float a_x, float a_y, float a_size, float a_grow, ImGuiMCP::ImU32 a_col)
		{
			float px = a_size / Glyph::kSize;
			for (int row = 0; row < Glyph::kSize; ++row) {
				const char* line = Glyph::kZan[row];
				for (int col = 0; col < Glyph::kSize;) {
					if (line[col] != '#') {
						++col;
						continue;
					}
					int end = col;
					while (end < Glyph::kSize && line[end] == '#') {
						++end;
					}
					ImGuiMCP::ImDrawListManager::AddRectFilled(a_dl,
						ImGuiMCP::ImVec2{ a_x + col * px - a_grow, a_y + row * px - a_grow },
						ImGuiMCP::ImVec2{ a_x + end * px + a_grow, a_y + (row + 1) * px + a_grow }, a_col, 0.0f, 0);
					col = end;
				}
			}
		}

		void __stdcall DrawMarkers()
		{
			auto& s = Settings::Get();
			if (!s.enabled || !s.markerEnabled || SKSEMenuFramework::IsAnyBlockingWindowOpened()) {
				return;
			}
			auto markers = Core::Get().Markers();
			auto io = ImGuiMCP::GetIO();
			if (markers.empty() || !io) {
				return;
			}
			float w = io->DisplaySize.x, h = io->DisplaySize.y;
			auto  dl = ImGuiMCP::GetForegroundDrawList();
			float t = static_cast<float>(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
			float pulse = 0.6f + 0.4f * std::sin(t * 6.0f);
			auto  alpha = static_cast<int>(255 * pulse);
			for (auto& m : markers) {
				float x = m.x * w + s.markerOffsetX - s.markerSize * 0.5f;
				float y = m.y * h + s.markerOffsetY - s.markerSize * 0.5f;
				DrawGlyph(dl, x, y, s.markerSize, s.markerSize * 0.05f, IM_COL32(40, 0, 0, alpha * 2 / 3));
				DrawGlyph(dl, x, y, s.markerSize, 0.0f, IM_COL32(s.markerR, s.markerG, s.markerB, alpha));
			}
		}

		// The menu framework swallows game input while it is open, so the new key is read
		// straight from Windows while the Settings page is being drawn.
		std::array<bool, 256> g_prevDown{};

		std::uint32_t VkToCode(int a_vk)
		{
			switch (a_vk) {
			case VK_RBUTTON: return 257;
			case VK_MBUTTON: return 258;
			case VK_XBUTTON1: return 259;
			case VK_XBUTTON2: return 260;
			default: break;
			}
			UINT sc = ::MapVirtualKeyW(static_cast<UINT>(a_vk), MAPVK_VK_TO_VSC_EX);
			if (!sc) {
				return 0;
			}
			std::uint32_t code = sc & 0xFF;
			if ((sc & 0xFF00) == 0xE000) {
				code |= 0x80;
			}
			// keys Windows reports without the extended bit but DirectInput keeps apart
			switch (a_vk) {
			case VK_RCONTROL: return 0x9D;
			case VK_RMENU: return 0xB8;
			case VK_DIVIDE: return 0xB5;
			case VK_INSERT: return 0xD2;
			case VK_DELETE: return 0xD3;
			case VK_HOME: return 0xC7;
			case VK_END: return 0xCF;
			case VK_PRIOR: return 0xC9;
			case VK_NEXT: return 0xD1;
			case VK_UP: return 0xC8;
			case VK_DOWN: return 0xD0;
			case VK_LEFT: return 0xCB;
			case VK_RIGHT: return 0xCD;
			default: return code;
			}
		}

		void SnapshotKeys()
		{
			for (int vk = 1; vk < 256; ++vk) {
				g_prevDown[vk] = (::GetAsyncKeyState(vk) & 0x8000) != 0;
			}
		}

		std::chrono::steady_clock::time_point g_lastPoll{};

		void PollCapture(Settings& s)
		{
			// page was closed and reopened mid-capture: don't take the key that reopened the menu
			auto now = std::chrono::steady_clock::now();
			bool stale = now - g_lastPoll > std::chrono::milliseconds(500);
			g_lastPoll = now;
			if (stale) {
				SnapshotKeys();
				return;
			}
			for (int vk = 1; vk < 256; ++vk) {
				bool down = (::GetAsyncKeyState(vk) & 0x8000) != 0;
				bool pressed = down && !g_prevDown[vk];
				g_prevDown[vk] = down;
				if (!pressed) {
					continue;
				}
				// left mouse clicks the UI; generic shift/ctrl/alt duplicate their left/right versions
				if (vk == VK_LBUTTON || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) {
					continue;
				}
				if (vk == VK_ESCAPE) {
					Input::CancelCapture();
					return;
				}
				if (auto code = VkToCode(vk)) {
					s.executeKey = code;
					s.Save();
					Input::CancelCapture();
					return;
				}
			}
		}

		void __stdcall RenderSettings()
		{
			auto& s = Settings::Get();
			bool  changed = false;
			changed |= ImGuiMCP::Checkbox("Enabled", &s.enabled);

			ImGuiMCP::SeparatorText("Execute");
			ImGuiMCP::Text("Execute key: %s  (code %u)", Input::KeyName(s.executeKey), s.executeKey);
			ImGuiMCP::SameLine();
			if (Input::IsCapturing()) {
				PollCapture(s);
				if (ImGuiMCP::Button("Press any key... (Esc = cancel)")) {
					Input::CancelCapture();
				}
			} else if (ImGuiMCP::Button("Change key")) {
				SnapshotKeys();
				g_lastPoll = std::chrono::steady_clock::now();
				Input::BeginCapture();
			}
			float pct = s.healthThreshold * 100.0f;
			if (ImGuiMCP::SliderFloat("Executable below health", &pct, 5.0f, 60.0f, "%.0f%%")) {
				s.healthThreshold = pct / 100.0f;
				changed = true;
			}
			changed |= ImGuiMCP::SliderFloat("Range", &s.range, 120.0f, 500.0f, "%.0f units");
			changed |= ImGuiMCP::SliderFloat("Aim cone", &s.cone, 20.0f, 180.0f, "%.0f deg");
			changed |= ImGuiMCP::Checkbox("Humanoid enemies only", &s.humanoidOnly);
			changed |= ImGuiMCP::Checkbox("Key also swings (off = press the key, then attack yourself)", &s.autoAttack);
			changed |= ImGuiMCP::SliderFloat("Execution window", &s.window, 0.5f, 5.0f, "%.1f s");
			ImGuiMCP::TextWrapped(
				"The key picks one killmove the game can play on that enemy right now and opens only that one; "
				"the attack then lets the game's own killmove system start it. If the game does not start it within the window, "
				"everything is put back and the enemy keeps its health.");

			ImGuiMCP::SeparatorText("Random");
			ImGuiMCP::TextWrapped("First a source is drawn by these weights, then an animation from it. Equal weights = each source equally often.");
			changed |= ImGuiMCP::SliderInt("Vanilla / VioLens", &s.weightVanilla, 0, 10);
			changed |= ImGuiMCP::SliderInt("Low-blow", &s.weightLowBlow, 0, 10);
			changed |= ImGuiMCP::SliderInt("Thicc Thighs", &s.weightThicc, 0, 10);
			changed |= ImGuiMCP::SliderInt("Don't repeat the last", &s.noRepeat, 0, 20, "%d animations");

			ImGuiMCP::SeparatorText("Marker");
			changed |= ImGuiMCP::Checkbox("Show the marker", &s.markerEnabled);
			changed |= ImGuiMCP::SliderFloat("Size", &s.markerSize, 12.0f, 64.0f, "%.0f px");
			changed |= ImGuiMCP::SliderFloat("Offset right", &s.markerOffsetX, -200.0f, 200.0f, "%.0f px");
			changed |= ImGuiMCP::SliderFloat("Offset down", &s.markerOffsetY, -200.0f, 200.0f, "%.0f px");
			changed |= ImGuiMCP::SliderFloat("Height above head", &s.markerHeight, -40.0f, 120.0f, "%.0f");
			changed |= ImGuiMCP::SliderInt("Red", &s.markerR, 0, 255);
			changed |= ImGuiMCP::SliderInt("Green", &s.markerG, 0, 255);
			changed |= ImGuiMCP::SliderInt("Blue", &s.markerB, 0, 255);

			ImGuiMCP::SeparatorText("Status");
			auto& core = Core::Get();
			auto  c = core.CountBySource();
			ImGuiMCP::Text("Killmoves found: %d vanilla / VioLens, %d Low-blow, %d Thicc Thighs", c[kVanilla], c[kLowBlow], c[kThicc]);
			int started[kSourceCount]{}, missed[kSourceCount]{};
			for (auto& km : core.All()) {
				started[km.source] += km.started;
				missed[km.source] += km.missed;
			}
			for (int i = 0; i < kSourceCount; ++i) {
				ImGuiMCP::Text("  %s: %d started, %d not started", SourceName(i), started[i], missed[i]);
			}
			ImGuiMCP::Text("Last: %s", core.LastResult().empty() ? "-" : core.LastResult().c_str());

			if (changed) {
				s.Save();
			}
		}
	}

	void Register()
	{
		if (!SKSEMenuFramework::IsInstalled()) {
			logger::warn("SKSE Menu Framework not found: no settings page and no marker");
			return;
		}
		SKSEMenuFramework::SetSection("Zan");
		SKSEMenuFramework::AddSectionItem("Settings", RenderSettings);
		SKSEMenuFramework::AddHudElement(DrawMarkers);
	}
}
