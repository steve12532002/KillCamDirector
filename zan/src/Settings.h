#pragma once

namespace Zan
{
	struct Settings
	{
		static Settings& Get()
		{
			static Settings s;
			return s;
		}

		bool          enabled{ true };
		std::uint32_t executeKey{ 0x2F };  // V (DirectInput scan code; mouse buttons 256+)
		float         healthThreshold{ 0.25f };
		float         range{ 260.0f };
		float         cone{ 70.0f };
		bool          humanoidOnly{ true };
		bool          autoAttack{ true };  // the key also swings at the target (off = key, then attack yourself)
		float         window{ 2.5f };      // seconds the next attack stays an execution

		// how often each source is chosen (relative); 0 = never
		int weightVanilla{ 1 };
		int weightLowBlow{ 1 };
		int weightThicc{ 1 };
		int noRepeat{ 4 };
		std::string blocked;  // editor IDs the player blocked (T-pose / broken), comma separated  // the last N animations are skipped while others are available

		bool        markerEnabled{ true };
		float       markerSize{ 26.0f };
		float       markerOffsetX{ 70.0f };
		float       markerOffsetY{ 0.0f };
		float       markerHeight{ 28.0f };
		int         markerR{ 255 }, markerG{ 110 }, markerB{ 110 };

		bool verbose{ false };

		void Load();
		void Save() const;
	};
}
