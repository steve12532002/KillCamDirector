#include "PCH.h"
#include "Settings.h"

#include <SimpleIni.h>

namespace KCD
{
	namespace
	{
		constexpr auto kBaseIni = "Data/SKSE/Plugins/KillCamDirector.ini";
		constexpr auto kUserIni = "Data/SKSE/Plugins/KillCamDirector_User.ini";
		constexpr auto kPresetDir = "Data/SKSE/Plugins/KillCamDirector/Presets";

		std::pair<float, float> Pair(const CSimpleIniA& a_ini, const char* a_sec, const char* a_key, float a_def)
		{
			const char* v = a_ini.GetValue(a_sec, a_key, nullptr);
			if (!v) {
				return { a_def, a_def };
			}
			std::string s{ v };
			auto        comma = s.find(',');
			try {
				if (comma == std::string::npos) {
					float f = std::stof(s);
					return { f, f };
				}
				return { std::stof(s.substr(0, comma)), std::stof(s.substr(comma + 1)) };
			} catch (...) {
				return { a_def, a_def };
			}
		}

		float F(const CSimpleIniA& a_ini, const char* a_sec, const char* a_key, float a_def)
		{
			return static_cast<float>(a_ini.GetDoubleValue(a_sec, a_key, a_def));
		}

		void ReadGeneral(Settings& s, const CSimpleIniA& ini)
		{
			s.enabled = ini.GetBoolValue("General", "bEnabled", s.enabled);
			s.preset = ini.GetValue("General", "sPreset", s.preset.c_str());
			s.toggleKey = static_cast<std::uint32_t>(ini.GetLongValue("General", "iToggleKey", s.toggleKey));
			s.nextPresetKey = static_cast<std::uint32_t>(ini.GetLongValue("General", "iNextPresetKey", s.nextPresetKey));
			s.notifications = ini.GetBoolValue("General", "bNotifications", s.notifications);
			s.forceThirdPerson = ini.GetBoolValue("General", "bForceThirdPerson", s.forceThirdPerson);
			s.maxPartnerDistance = F(ini, "General", "fMaxPartnerDistance", s.maxPartnerDistance);
			s.smoothing = F(ini, "General", "fSmoothing", s.smoothing);
			s.blendOut = F(ini, "General", "fBlendOut", s.blendOut);
			s.stopDelay = F(ini, "General", "fStopDelay", s.stopDelay);
			s.distanceScale = F(ini, "Framing", "fDistanceScale", s.distanceScale);
			s.autoZoom = ini.GetBoolValue("Framing", "bAutoZoom", s.autoZoom);
			s.preferFace = ini.GetBoolValue("Framing", "bPreferFace", s.preferFace);
			s.minHeight = F(ini, "Framing", "fMinHeight", s.minHeight);
			s.collision = ini.GetBoolValue("Collision", "bEnabled", s.collision);
			s.collisionMargin = F(ini, "Collision", "fMargin", s.collisionMargin);
			s.collisionLayer = static_cast<int>(ini.GetLongValue("Collision", "iLayer", s.collisionLayer));
			s.letterboxOverride = F(ini, "Override", "fLetterbox", s.letterboxOverride);
			s.slowMode = static_cast<int>(ini.GetLongValue("SlowMotion", "iMode", s.slowMode));
			s.slowScale = F(ini, "SlowMotion", "fScale", s.slowScale);
			s.slowDuration = F(ini, "SlowMotion", "fDuration", s.slowDuration);
			s.axisMode = static_cast<int>(ini.GetLongValue("Debug", "iAxisMode", s.axisMode));
			s.verbose = ini.GetBoolValue("Debug", "bVerbose", s.verbose);
		}
	}

	void Settings::Load()
	{
		for (auto path : { kBaseIni, kUserIni }) {
			CSimpleIniA ini;
			ini.SetUnicode();
			if (ini.LoadFile(path) >= 0) {
				ReadGeneral(*this, ini);
				logger::info("Loaded {}", path);
			}
		}
		LoadPresets();
	}

	void Settings::SaveUser()
	{
		CSimpleIniA ini;
		ini.SetUnicode();
		ini.SetBoolValue("General", "bEnabled", enabled);
		ini.SetValue("General", "sPreset", preset.c_str());
		ini.SetDoubleValue("Override", "fLetterbox", letterboxOverride);
		ini.SetDoubleValue("Framing", "fDistanceScale", distanceScale);
		ini.SetBoolValue("Framing", "bAutoZoom", autoZoom);
		ini.SetBoolValue("Framing", "bPreferFace", preferFace);
		ini.SetBoolValue("Collision", "bEnabled", collision);
		ini.SetLongValue("SlowMotion", "iMode", slowMode);
		ini.SetDoubleValue("SlowMotion", "fScale", slowScale);
		ini.SetDoubleValue("SlowMotion", "fDuration", slowDuration);
		if (ini.SaveFile(kUserIni) < 0) {
			logger::warn("Could not save {}", kUserIni);
		}
	}

	void Settings::LoadPresets()
	{
		presets.clear();
		std::error_code ec;
		if (!std::filesystem::exists(kPresetDir, ec)) {
			logger::warn("Preset folder {} not found", kPresetDir);
			return;
		}
		std::vector<std::filesystem::path> files;
		for (auto& e : std::filesystem::directory_iterator(kPresetDir, ec)) {
			if (e.is_regular_file() && e.path().extension() == ".ini") {
				files.push_back(e.path());
			}
		}
		std::sort(files.begin(), files.end());
		for (auto& f : files) {
			CSimpleIniA ini;
			ini.SetUnicode();
			if (ini.LoadFile(f.string().c_str()) < 0) {
				continue;
			}
			Preset p;
			p.file = f.filename().string();
			p.name = ini.GetValue("Preset", "Name", f.stem().string().c_str());
			p.letterbox = F(ini, "Preset", "Letterbox", p.letterbox);
			for (int i = 1; i <= 16; ++i) {
				auto sec = "Shot" + std::to_string(i);
				if (ini.GetSectionSize(sec.c_str()) < 0) {
					break;
				}
				Shot s;
				const char* c = sec.c_str();
				s.duration = F(ini, c, "Duration", s.duration);
				std::tie(s.yaw0, s.yaw1) = Pair(ini, c, "Yaw", 90.0f);
				std::tie(s.dist0, s.dist1) = Pair(ini, c, "Distance", 180.0f);
				std::tie(s.height0, s.height1) = Pair(ini, c, "Height", 15.0f);
				std::tie(s.fov0, s.fov1) = Pair(ini, c, "FOV", 0.0f);
				s.lookAt = F(ini, c, "LookAt", s.lookAt);
				s.blend = F(ini, c, "Blend", s.blend);
				std::string anchor = ini.GetValue(c, "Anchor", "pair");
				s.anchor = (_stricmp(anchor.c_str(), "player") == 0) ? 1 : 0;
				std::string side = ini.GetValue(c, "Side", "random");
				std::transform(side.begin(), side.end(), side.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
				s.side = side == "right" ? 1 : side == "left" ? -1 : side == "same" ? 2 : 0;
				p.shots.push_back(s);
			}
			if (p.shots.empty()) {
				p.shots.push_back(Shot{});
			}
			logger::info("Preset '{}' ({}) - {} shots", p.name, p.file, p.shots.size());
			presets.push_back(std::move(p));
		}
	}

	const Preset* Settings::FindPreset(const std::string& a_name) const
	{
		for (auto& p : presets) {
			if (_stricmp(p.name.c_str(), a_name.c_str()) == 0 || _stricmp(p.file.c_str(), a_name.c_str()) == 0) {
				return &p;
			}
		}
		return nullptr;
	}

	const Preset* Settings::PickPreset()
	{
		if (presets.empty()) {
			return nullptr;
		}
		if (_stricmp(preset.c_str(), "Random") != 0) {
			if (auto p = FindPreset(preset)) {
				return p;
			}
		}
		static std::mt19937 rng{ std::random_device{}() };
		std::uniform_int_distribution<std::size_t> d(0, presets.size() - 1);
		return &presets[d(rng)];
	}
}
