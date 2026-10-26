#pragma once

namespace KCD
{
	// One camera set-up inside a preset. Every "a,b" value animates from a to b over the shot.
	struct Shot
	{
		float duration{ 2.0f };   // game seconds; the last shot holds until the killmove ends
		float yaw0{ 90.0f };      // camera angle around the pair, degrees.
		float yaw1{ 90.0f };      //   0 = behind the player looking at the enemy, 90 = side, 180 = behind the enemy
		float dist0{ 180.0f };    // distance from the pair's centre (game units, ~1.4 cm each)
		float dist1{ 180.0f };
		float height0{ 15.0f };   // height relative to the pair's chest height
		float height1{ 15.0f };
		float lookAt{ 0.5f };     // 0 = look at the player, 1 = look at the enemy
		float fov0{ 0.0f };       // 0 = keep the game's FOV
		float fov1{ 0.0f };
		int   side{ 0 };          // 0 = random, 1 = right, -1 = left, 2 = same side as previous shot
		float blend{ 0.35f };     // seconds to move from the previous shot; 0 = hard cut
	};

	struct Preset
	{
		std::string       name;
		std::string       file;
		float             letterbox{ 0.10f };    // fraction of screen height per bar
		bool              slowMotion{ true };
		float             slowScale{ 0.35f };    // time multiplier while slow
		bool              slowOnHit{ true };     // slow down on hit / kill animation events
		float             slowDuration{ 0.7f };  // real seconds per hit trigger
		float             slowStart{ -1.0f };    // optional fixed window (game seconds from start)
		float             slowEnd{ -1.0f };
		std::vector<Shot> shots;
	};

	class Settings
	{
	public:
		static Settings& Get()
		{
			static Settings s;
			return s;
		}

		void Load();
		void SaveUser();
		void LoadPresets();

		const Preset* PickPreset();  // honours presetMode
		const Preset* FindPreset(const std::string& a_name) const;

		// [General]
		bool        enabled{ true };
		std::string preset{ "Random" };  // preset name or "Random"
		std::uint32_t toggleKey{ 0xD2 };      // Insert
		std::uint32_t nextPresetKey{ 0xC9 };  // Page Up
		bool        notifications{ true };
		bool        forceThirdPerson{ true };
		float       maxPartnerDistance{ 600.0f };
		float       smoothing{ 0.10f };   // seconds, camera follow smoothing
		float       blendOut{ 0.45f };    // seconds to hand the camera back
		// overrides: -1 = use preset
		float letterboxOverride{ -1.0f };
		int   slowMotionOverride{ -1 };
		float slowScaleOverride{ -1.0f };
		// [Debug]
		int  axisMode{ -1 };  // -1 auto-detect, 0 = +Y forward, 1 = +X forward
		bool verbose{ false };

		std::vector<Preset> presets;

	private:
		Settings() = default;
	};
}
