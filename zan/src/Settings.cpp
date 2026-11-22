#include "PCH.h"
#include "Settings.h"

#include <SimpleIni.h>

namespace Zan
{
	namespace
	{
		constexpr auto kDefaults = L"Data/SKSE/Plugins/Zan.ini";
		constexpr auto kUser = L"Data/SKSE/Plugins/Zan_User.ini";

		float F(CSimpleIniA& a_ini, const char* a_sec, const char* a_key, float a_def)
		{
			return static_cast<float>(a_ini.GetDoubleValue(a_sec, a_key, a_def));
		}

		void Read(Settings& s, const wchar_t* a_path)
		{
			CSimpleIniA ini;
			ini.SetUnicode();
			if (ini.LoadFile(a_path) < 0) {
				return;
			}
			s.enabled = ini.GetBoolValue("General", "bEnabled", s.enabled);
			s.executeKey = static_cast<std::uint32_t>(ini.GetLongValue("General", "iExecuteKey", static_cast<long>(s.executeKey)));
			s.healthThreshold = F(ini, "General", "fHealthThreshold", s.healthThreshold);
			s.range = F(ini, "General", "fRange", s.range);
			s.cone = F(ini, "General", "fCone", s.cone);
			s.humanoidOnly = ini.GetBoolValue("General", "bHumanoidOnly", s.humanoidOnly);
			s.mode = static_cast<int>(ini.GetLongValue("General", "iPickMode", s.mode));
			s.window = F(ini, "General", "fWindow", s.window);
			s.weightVanilla = static_cast<int>(ini.GetLongValue("Random", "iVanilla", s.weightVanilla));
			s.weightLowBlow = static_cast<int>(ini.GetLongValue("Random", "iLowBlow", s.weightLowBlow));
			s.weightThicc = static_cast<int>(ini.GetLongValue("Random", "iThicc", s.weightThicc));
			s.noRepeat = static_cast<int>(ini.GetLongValue("Random", "iNoRepeat", s.noRepeat));
			s.blocked = ini.GetValue("Random", "sBlocked", s.blocked.c_str());
			s.markerEnabled = ini.GetBoolValue("Marker", "bEnabled", s.markerEnabled);
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
		Read(*this, kDefaults);
		Read(*this, kUser);
		healthThreshold = std::clamp(healthThreshold, 0.01f, 1.0f);
		noRepeat = std::clamp(noRepeat, 0, 20);
	}

	void Settings::Save() const
	{
		CSimpleIniA ini;
		ini.SetUnicode();
		ini.SetBoolValue("General", "bEnabled", enabled);
		ini.SetLongValue("General", "iExecuteKey", static_cast<long>(executeKey));
		ini.SetDoubleValue("General", "fHealthThreshold", healthThreshold);
		ini.SetDoubleValue("General", "fRange", range);
		ini.SetDoubleValue("General", "fCone", cone);
		ini.SetBoolValue("General", "bHumanoidOnly", humanoidOnly);
		ini.SetLongValue("General", "iPickMode", mode);
		ini.SetDoubleValue("General", "fWindow", window);
		ini.SetLongValue("Random", "iVanilla", weightVanilla);
		ini.SetLongValue("Random", "iLowBlow", weightLowBlow);
		ini.SetLongValue("Random", "iThicc", weightThicc);
		ini.SetLongValue("Random", "iNoRepeat", noRepeat);
		ini.SetValue("Random", "sBlocked", blocked.c_str());
		ini.SetBoolValue("Marker", "bEnabled", markerEnabled);
		ini.SetDoubleValue("Marker", "fSize", markerSize);
		ini.SetDoubleValue("Marker", "fOffsetX", markerOffsetX);
		ini.SetDoubleValue("Marker", "fOffsetY", markerOffsetY);
		ini.SetDoubleValue("Marker", "fHeight", markerHeight);
		ini.SetLongValue("Marker", "iRed", markerR);
		ini.SetLongValue("Marker", "iGreen", markerG);
		ini.SetLongValue("Marker", "iBlue", markerB);
		ini.SetBoolValue("Debug", "bVerbose", verbose);
		ini.SaveFile(kUser);
	}
}
