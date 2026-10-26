#include "PCH.h"

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <windows.h>

#include "SKSEMenuFramework.h"

#include "Director.h"
#include "Input.h"
#include "Settings.h"
#include "UI.h"

namespace KCD::UI
{
	namespace
	{
		void __stdcall DrawLetterbox()
		{
			float lb = Director::Get().CurrentLetterbox();
			if (lb <= 0.001f) {
				return;
			}
			auto io = ImGuiMCP::GetIO();
			if (!io) {
				return;
			}
			float w = io->DisplaySize.x;
			float h = io->DisplaySize.y;
			float bar = h * lb;
			auto  dl = ImGuiMCP::GetBackgroundDrawList();
			ImGuiMCP::ImDrawListManager::AddRectFilled(dl, ImGuiMCP::ImVec2{ 0, 0 }, ImGuiMCP::ImVec2{ w, bar }, IM_COL32(0, 0, 0, 255), 0.0f, 0);
			ImGuiMCP::ImDrawListManager::AddRectFilled(dl, ImGuiMCP::ImVec2{ 0, h - bar }, ImGuiMCP::ImVec2{ w, h }, IM_COL32(0, 0, 0, 255), 0.0f, 0);
		}

		void __stdcall RenderSettings()
		{
			auto& s = Settings::Get();
			bool  changed = false;

			changed |= ImGuiMCP::Checkbox("Enabled (hotkey toggles too)", &s.enabled);

			// preset picker
			std::vector<std::string> names{ "Random" };
			for (auto& p : s.presets) {
				names.push_back(p.name);
			}
			int current = 0;
			for (int i = 0; i < static_cast<int>(names.size()); ++i) {
				if (_stricmp(names[i].c_str(), s.preset.c_str()) == 0) {
					current = i;
				}
			}
			std::vector<const char*> items;
			for (auto& n : names) {
				items.push_back(n.c_str());
			}
			if (ImGuiMCP::Combo("Preset", &current, items.data(), static_cast<int>(items.size()))) {
				s.preset = names[current];
				changed = true;
			}

			ImGuiMCP::SeparatorText("Framing");
			changed |= ImGuiMCP::SliderFloat("Camera distance", &s.distanceScale, 0.5f, 1.5f, "%.2fx");
			changed |= ImGuiMCP::Checkbox("Auto zoom (closer when the fighters are close)", &s.autoZoom);
			changed |= ImGuiMCP::Checkbox("Prefer the side that shows your face", &s.preferFace);
			changed |= ImGuiMCP::Checkbox("Avoid walls (swing round / move in when blocked)", &s.collision);

			ImGuiMCP::SeparatorText("Letterbox");
			bool lbOverride = s.letterboxOverride >= 0.0f;
			if (ImGuiMCP::Checkbox("Override preset letterbox", &lbOverride)) {
				s.letterboxOverride = lbOverride ? 0.10f : -1.0f;
				changed = true;
			}
			if (lbOverride) {
				float pct = s.letterboxOverride * 100.0f;
				if (ImGuiMCP::SliderFloat("Bar height (% of screen)", &pct, 0.0f, 20.0f, "%.0f%%")) {
					s.letterboxOverride = pct / 100.0f;
					changed = true;
				}
			}

			ImGuiMCP::SeparatorText("Slow motion (all presets)");
			const char* slowItems[] = { "Off", "On each hit / kill (finisher)", "Whole killmove" };
			int         slowIdx = std::clamp(s.slowMode, 0, 2);
			if (ImGuiMCP::Combo("Slow motion", &slowIdx, slowItems, 3)) {
				s.slowMode = slowIdx;
				changed = true;
			}
			if (s.slowMode > 0) {
				changed |= ImGuiMCP::SliderFloat("Game speed while slow", &s.slowScale, 0.1f, 0.9f, "%.2fx");
			}
			if (s.slowMode == 1) {
				changed |= ImGuiMCP::SliderFloat("Seconds per hit", &s.slowDuration, 0.2f, 2.5f, "%.1f s");
			}

			ImGuiMCP::SeparatorText("Presets");
			if (ImGuiMCP::Button("Reload presets from disk")) {
				s.LoadPresets();
			}
			ImGuiMCP::Text("%d presets loaded", static_cast<int>(s.presets.size()));
			if (auto p = Director::Get().CurrentPreset(); p && Director::Get().IsActive()) {
				ImGuiMCP::Text("Now filming with: %s", p->name.c_str());
			}

			if (changed) {
				s.SaveUser();
			}
		}
	}

	void Register()
	{
		if (!SKSEMenuFramework::IsInstalled()) {
			logger::warn("SKSE Menu Framework not found: no settings page and no letterbox bars");
			return;
		}
		SKSEMenuFramework::SetSection("KillCam Director");
		SKSEMenuFramework::AddSectionItem("Settings", RenderSettings);
		SKSEMenuFramework::AddHudElement(DrawLetterbox);
		logger::info("SKSE Menu Framework page registered");
	}
}
