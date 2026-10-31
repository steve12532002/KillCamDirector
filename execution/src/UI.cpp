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

#include "Executor.h"
#include "Finishers.h"
#include "Settings.h"

namespace ED::UI
{
	namespace
	{
		char g_filter[64] = "";
		bool g_showAll = false;
		int  g_weapon = -1;  // -1 = all weapon classes at once
		char g_presetName[64] = "";
		std::string g_presetMsg;

		// Draws the built-in glyph bitmap as horizontal runs of rectangles
		void DrawGlyph(ImGuiMCP::ImDrawList* a_dl, float a_x, float a_y, float a_size, float a_grow, ImGuiMCP::ImU32 a_col)
		{
			float px = a_size / Glyph::kSize;
			for (int row = 0; row < Glyph::kSize; ++row) {
				const char* line = Glyph::kKill[row];
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
			if (!s.markerEnabled || SKSEMenuFramework::IsAnyBlockingWindowOpened()) {
				return;
			}
			auto markers = Executor::Get().Markers();
			if (markers.empty()) {
				return;
			}
			static bool loggedDraw = false;
			if (!loggedDraw) {
				loggedDraw = true;
				logger::info("Drawing {} marker(s), first at {:.3f}, {:.3f}", markers.size(), markers[0].x, markers[0].y);
			}
			auto io = ImGuiMCP::GetIO();
			if (!io) {
				return;
			}
			float w = io->DisplaySize.x, h = io->DisplaySize.y;
			auto  dl = ImGuiMCP::GetForegroundDrawList();
			auto  font = ImGuiMCP::GetFont();
			float t = static_cast<float>(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
			float pulse = 0.55f + 0.45f * std::sin(t * 6.0f);
			auto  alpha = static_cast<int>(255 * pulse);
			const char* text = s.markerText.c_str();
			// the menu framework's font has no CJK glyphs, so the default text uses a built-in bitmap
			bool useGlyph = s.markerText == "\xE6\xAE\xBA" || s.markerText.empty();
			for (auto& m : markers) {
				ImGuiMCP::ImVec2 pos{ m.x * w + s.markerOffsetX - s.markerSize * 0.5f, m.y * h + s.markerOffsetY - s.markerSize * 0.5f };
				if (useGlyph) {
					DrawGlyph(dl, pos.x, pos.y, s.markerSize, s.markerSize * 0.05f, IM_COL32(40, 0, 0, alpha * 2 / 3));
					DrawGlyph(dl, pos.x, pos.y, s.markerSize, 0.0f, IM_COL32(s.markerR, s.markerG, s.markerB, alpha));
					continue;
				}
				// soft dark outline so the pale red reads on bright backgrounds
				for (int dx = -1; dx <= 1; ++dx) {
					for (int dy = -1; dy <= 1; ++dy) {
						if (dx || dy) {
							ImGuiMCP::ImDrawListManager::AddText(dl, font, s.markerSize, ImGuiMCP::ImVec2{ pos.x + dx * 1.5f, pos.y + dy * 1.5f },
								IM_COL32(40, 0, 0, alpha / 2), text, nullptr, 0.0f, nullptr);
						}
					}
				}
				ImGuiMCP::ImDrawListManager::AddText(dl, font, s.markerSize, pos, IM_COL32(s.markerR, s.markerG, s.markerB, alpha), text, nullptr, 0.0f, nullptr);
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
					s.SaveUser();
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
			changed |= ImGuiMCP::SliderFloat("Execute range", &s.executeRange, 120.0f, 500.0f, "%.0f units");
			changed |= ImGuiMCP::Checkbox("Stop random killmoves when YOU attack (NPC killmoves stay)", &s.blockRandomPlayerKillmoves);
			changed |= ImGuiMCP::Checkbox("Allow essential / protected NPCs", &s.allowEssential);
			changed |= ImGuiMCP::Checkbox("Prefer finishers made for your weapon", &s.preferWeaponMatch);

			ImGuiMCP::SeparatorText("Executable enemies");
			changed |= ImGuiMCP::Checkbox("Stagger them while executable", &s.staggerEnabled);
			if (s.staggerEnabled) {
				changed |= ImGuiMCP::SliderFloat("Stagger every", &s.staggerInterval, 1.0f, 6.0f, "%.1f s");
			}
			changed |= ImGuiMCP::Checkbox("Show the marker above their head", &s.markerEnabled);
			if (s.markerEnabled) {
				changed |= ImGuiMCP::SliderFloat("Marker size", &s.markerSize, 12.0f, 64.0f, "%.0f px");
				changed |= ImGuiMCP::SliderFloat("Marker offset X", &s.markerOffsetX, -200.0f, 200.0f, "%.0f px");
				changed |= ImGuiMCP::SliderFloat("Marker offset Y", &s.markerOffsetY, -200.0f, 200.0f, "%.0f px");
				changed |= ImGuiMCP::SliderFloat("Marker height above head", &s.markerHeight, 0.0f, 80.0f, "%.0f units");
			}
			ImGuiMCP::Text("Executable right now: %d", static_cast<int>(Executor::Get().MarkedCount()));
			if (!Executor::Get().LastResult().empty()) {
				ImGuiMCP::Text("Last execution: %s", Executor::Get().LastResult().c_str());
			}
			ImGuiMCP::TextDisabled("(turning the random-killmove block on or off takes effect after restarting the game)");
			if (changed) {
				s.SaveUser();
			}
		}

		void __stdcall RenderFinishers()
		{
			auto& cat = Catalog::Get();
			auto& all = cat.All();
			ImGuiMCP::TextWrapped(
				"Every paired animation the game has loaded. Untick to stop using it, change its category if it was guessed wrong, "
				"or press Next to use it on your next execution. 'Fails' counts animations that refused to start per weapon "
				"(Unarmed/1H/2H/Ranged/Other) - after 2 failures with a weapon an animation is marked auto-off and no longer picked "
				"for that weapon (Reset gives it another chance; Test still plays it). "
				"'Chance' is how likely an animation is picked compared with the others that fit (100 = normal, 50 = half as often, "
				"0 = never at random, only with Next). On and Chance are kept separately for each weapon type - pick one below. "
				"Test plays the animation on the nearest character as soon as you close this menu.");

			ImGuiMCP::SeparatorText("Presets");
			ImGuiMCP::SetNextItemWidth(220.0f);
			ImGuiMCP::InputText("##presetname", g_presetName, sizeof(g_presetName));
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button("Save preset")) {
				g_presetMsg = cat.SavePreset(g_presetName) ? std::string("Saved ") + g_presetName : "Enter a name without \\ / : * ? \" < > |";
			}
			for (auto& name : cat.ListPresets()) {
				ImGuiMCP::PushID(("preset_" + name).c_str());
				if (ImGuiMCP::Button("Load")) {
					g_presetMsg = cat.LoadPreset(name) ? "Loaded " + name : "Could not load " + name;
				}
				ImGuiMCP::SameLine();
				if (ImGuiMCP::Button("Delete")) {
					cat.DeletePreset(name);
					g_presetMsg = "Deleted " + name;
				}
				ImGuiMCP::SameLine();
				ImGuiMCP::Text("%s", name.c_str());
				ImGuiMCP::PopID();
			}
			if (!g_presetMsg.empty()) {
				ImGuiMCP::TextDisabled("%s", g_presetMsg.c_str());
			}

			ImGuiMCP::SeparatorText("Weapon type");
			ImGuiMCP::RadioButton("All", &g_weapon, -1);
			for (int w = 0; w < kWeaponClassCount; ++w) {
				ImGuiMCP::SameLine();
				ImGuiMCP::RadioButton(WeaponClassName(w), &g_weapon, w);
			}
			if (auto test = Executor::Get().PendingTest(); !test.empty()) {
				ImGuiMCP::TextColored(ImGuiMCP::ImVec4{ 1.0f, 0.6f, 0.6f, 1.0f }, "Test queued: %s - close the menu", test.c_str());
			}
			ImGuiMCP::Separator();
			ImGuiMCP::InputText("Filter", g_filter, sizeof(g_filter));
			ImGuiMCP::Checkbox("Also show non-killmove and creature animations", &g_showAll);
			std::string filter = g_filter;
			std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

			bool changed = false;
			const char* cats[] = { "Front", "Back", "Sneak", "Bleedout", "Any" };
			auto flags = ImGuiMCP::ImGuiTableFlags_Borders | ImGuiMCP::ImGuiTableFlags_RowBg | ImGuiMCP::ImGuiTableFlags_Resizable | ImGuiMCP::ImGuiTableFlags_SizingFixedFit;
			if (ImGuiMCP::BeginTable("finishers", 9, flags)) {
				ImGuiMCP::TableSetupColumn("On");
				ImGuiMCP::TableSetupColumn("Try");
				ImGuiMCP::TableSetupColumn("Animation event");
				ImGuiMCP::TableSetupColumn("Chance");
				ImGuiMCP::TableSetupColumn("Category");
				ImGuiMCP::TableSetupColumn("Needs");
				ImGuiMCP::TableSetupColumn("From");
				ImGuiMCP::TableSetupColumn("OK / Fails");
				ImGuiMCP::TableSetupColumn("Stats");
				ImGuiMCP::TableHeadersRow();
				for (auto& f : all) {
					if (!g_showAll && (!f.killmove || f.creature)) {
						continue;
					}
					if (!filter.empty()) {
						std::string hay = f.key + " " + f.editorID;
						for (auto& src : f.sources) hay += " " + src;
						std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
						if (hay.find(filter) == std::string::npos) {
							continue;
						}
					}
					ImGuiMCP::PushID(f.key.c_str());
					ImGuiMCP::TableNextRow();
					ImGuiMCP::TableNextColumn();
					{
						bool on = g_weapon < 0 ? std::all_of(f.on.begin(), f.on.end(), [](bool v) { return v; }) : f.on[g_weapon];
						if (ImGuiMCP::Checkbox("##on", &on)) {
							if (g_weapon < 0) {
								f.on.fill(on);
							} else {
								f.on[g_weapon] = on;
							}
							changed = true;
						}
					}
					ImGuiMCP::TableNextColumn();
					if (ImGuiMCP::Button("Test")) {
						Executor::Get().RequestTest(f.key);
					}
					ImGuiMCP::SameLine();
					if (ImGuiMCP::Button(Executor::Get().ForcedNext() == f.key ? "Queued" : "Next")) {
						Executor::Get().ForceNext(f.key);
					}
					ImGuiMCP::TableNextColumn();
					ImGuiMCP::Text("%s", f.event.c_str());
					ImGuiMCP::TableNextColumn();
					ImGuiMCP::SetNextItemWidth(110.0f);
					{
						bool mixed = g_weapon < 0 && std::any_of(f.chance.begin(), f.chance.end(), [&](int v) { return v != f.chance[0]; });
						int  ch = g_weapon < 0 ? f.chance[0] : f.chance[g_weapon];
						if (ImGuiMCP::SliderInt("##chance", &ch, 0, 100, mixed ? "mixed" : "%d%%")) {
							if (g_weapon < 0) {
								f.chance.fill(ch);
							} else {
								f.chance[g_weapon] = ch;
							}
							changed = true;
						}
					}
					ImGuiMCP::TableNextColumn();
					int c = static_cast<int>(f.GetCategory());
					ImGuiMCP::SetNextItemWidth(120.0f);
					if (ImGuiMCP::Combo("##cat", &c, cats, 5)) {
						f.userCategory = c;
						changed = true;
					}
					ImGuiMCP::TableNextColumn();
					std::string needs;
					if (f.femaleAttacker) needs += "female ";
					if (f.maleAttacker) needs += "male ";
					if (f.hasWeaponTags) {
						for (int w = 0; w < kWeaponClassCount; ++w) {
							if (f.weaponOk[w]) needs += std::string(WeaponClassName(w)) + " ";
						}
					}
					if (f.creature) needs += "creature ";
					ImGuiMCP::Text("%s", needs.empty() ? "-" : needs.c_str());
					ImGuiMCP::TableNextColumn();
					std::string from;
					for (auto& src : f.sources) from += (from.empty() ? "" : ", ") + src;
					ImGuiMCP::Text("%s", from.c_str());
					ImGuiMCP::TableNextColumn();
					ImGuiMCP::Text("%d / %d %d %d %d %d", f.success, f.fails[0], f.fails[1], f.fails[2], f.fails[3], f.fails[4]);
					{
						std::string off;
						for (int w = 0; w < kWeaponClassCount; ++w) {
							if ((g_weapon < 0 || g_weapon == w) && f.AutoExcluded(w)) {
								off += std::string(off.empty() ? "" : " ") + WeaponClassName(w);
							}
						}
						if (!off.empty()) {
							ImGuiMCP::SameLine();
							ImGuiMCP::TextColored(ImGuiMCP::ImVec4{ 1.0f, 0.5f, 0.5f, 1.0f }, "auto-off: %s", off.c_str());
						}
					}
					ImGuiMCP::TableNextColumn();
					if (ImGuiMCP::Button("Reset")) {
						f.fails.fill(0);
						f.success = 0;
						changed = true;
					}
					ImGuiMCP::PopID();
				}
				ImGuiMCP::EndTable();
			}
			if (changed) {
				cat.SaveUser();
			}
		}
	}

	void Register()
	{
		if (!SKSEMenuFramework::IsInstalled()) {
			logger::warn("SKSE Menu Framework not found: no settings pages and no executable marker");
			return;
		}
		SKSEMenuFramework::SetSection("Execution Director");
		SKSEMenuFramework::AddSectionItem("Settings", RenderSettings);
		SKSEMenuFramework::AddSectionItem("Finishers", RenderFinishers);
		SKSEMenuFramework::AddHudElement(DrawMarkers);
	}
}
