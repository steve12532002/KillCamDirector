#include "PCH.h"
#include "Settings.h"

#include <SimpleIni.h>

namespace ED
{
	namespace
	{
		constexpr auto kBaseIni = "Data/SKSE/Plugins/ExecutionDirector.ini";
		constexpr auto kUserIni = "Data/SKSE/Plugins/ExecutionDirector_User.ini";

		float F(const CSimpleIniA& a_ini, const char* a_sec, const char* a_key, float a_def)
		{
			return static_cast<float>(a_ini.GetDoubleValue(a_sec, a_key, a_def));
		}

		void Read(Settings& s, const CSimpleIniA& ini)
		{
			s.enabled = ini.GetBoolValue("General", "bEnabled", s.enabled);
			s.executeKey = static_cast<std::uint32_t>(ini.GetLongValue("General", "iExecuteKey", s.executeKey));
			s.healthThreshold = F(ini, "General", "fHealthThreshold", s.healthThreshold);
			s.executeRange = F(ini, "General", "fExecuteRange", s.executeRange);
			s.executeCone = F(ini, "General", "fExecuteCone", s.executeCone);
			s.markRange = F(ini, "General", "fMarkRange", s.markRange);
			s.blockRandomPlayerKillmoves = ini.GetBoolValue("General", "bBlockRandomPlayerKillmoves", s.blockRandomPlayerKillmoves);
			s.humanoidOnly = ini.GetBoolValue("General", "bHumanoidOnly", s.humanoidOnly);
			s.allowEssential = ini.GetBoolValue("General", "bAllowEssential", s.allowEssential);
			s.ensureKill = ini.GetBoolValue("General", "bEnsureKill", s.ensureKill);
			s.maxRetries = static_cast<int>(ini.GetLongValue("General", "iMaxRetries", s.maxRetries));
			s.preferWeaponMatch = ini.GetBoolValue("General", "bPreferWeaponMatch", s.preferWeaponMatch);
			s.respectSexTags = ini.GetBoolValue("General", "bRespectSexTags", s.respectSexTags);
			s.notifications = ini.GetBoolValue("General", "bNotifications", s.notifications);

			s.staggerEnabled = ini.GetBoolValue("Stagger", "bEnabled", s.staggerEnabled);
			s.staggerInterval = F(ini, "Stagger", "fInterval", s.staggerInterval);
			s.staggerMagnitude = F(ini, "Stagger", "fMagnitude", s.staggerMagnitude);
			s.staggerWindow = F(ini, "Stagger", "fWindow", s.staggerWindow);

			s.markerEnabled = ini.GetBoolValue("Marker", "bEnabled", s.markerEnabled);
			s.markerText = ini.GetValue("Marker", "sText", s.markerText.c_str());
			s.markerSize = F(ini, "Marker", "fSize", s.markerSize);
			s.markerOffsetX = F(ini, "Marker", "fOffsetX", s.markerOffsetX);
			s.markerOffsetY = F(ini, "Marker", "fOffsetY", s.markerOffsetY);
			s.markerHeight = F(ini, "Marker", "fHeight", s.markerHeight);
			s.markerR = static_cast<int>(ini.GetLongValue("Marker", "iRed", s.markerR));
			s.markerG = static_cast<int>(ini.GetLongValue("Marker", "iGreen", s.markerG));
			s.markerB = static_cast<int>(ini.GetLongValue("Marker", "iBlue", s.markerB));

			s.verbose = ini.GetBoolValue("Debug", "bVerbose", s.verbose);
		}
	}

	void Settings::Load()
	{
		for (auto path : { kBaseIni, kUserIni }) {
			CSimpleIniA ini;
			ini.SetUnicode();
			if (ini.LoadFile(path) >= 0) {
				Read(*this, ini);
				logger::info("Loaded {}", path);
			}
		}
	}

	void Settings::SaveUser()
	{
		CSimpleIniA ini;
		ini.SetUnicode();
		ini.SetBoolValue("General", "bEnabled", enabled);
		ini.SetLongValue("General", "iExecuteKey", static_cast<long>(executeKey));
		ini.SetDoubleValue("General", "fHealthThreshold", healthThreshold);
		ini.SetDoubleValue("General", "fExecuteRange", executeRange);
		ini.SetBoolValue("General", "bBlockRandomPlayerKillmoves", blockRandomPlayerKillmoves);
		ini.SetBoolValue("General", "bAllowEssential", allowEssential);
		ini.SetBoolValue("General", "bPreferWeaponMatch", preferWeaponMatch);
		ini.SetBoolValue("Stagger", "bEnabled", staggerEnabled);
		ini.SetDoubleValue("Stagger", "fInterval", staggerInterval);
		ini.SetBoolValue("Marker", "bEnabled", markerEnabled);
		ini.SetDoubleValue("Marker", "fSize", markerSize);
		ini.SetDoubleValue("Marker", "fOffsetX", markerOffsetX);
		ini.SetDoubleValue("Marker", "fOffsetY", markerOffsetY);
		if (ini.SaveFile(kUserIni) < 0) {
			logger::warn("Could not save {}", kUserIni);
		}
	}
}
