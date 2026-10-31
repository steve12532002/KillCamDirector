#pragma once

namespace ED
{
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

		// [General]
		bool          enabled{ true };
		std::uint32_t executeKey{ 0x2F };  // V
		float         healthThreshold{ 0.25f };
		float         executeRange{ 280.0f };
		float         executeCone{ 75.0f };  // degrees either side of where you face
		float         markRange{ 3000.0f };
		bool          blockRandomPlayerKillmoves{ true };
		bool          humanoidOnly{ true };
		bool          allowEssential{ false };
		bool          ensureKill{ true };
		int           maxRetries{ 3 };
		bool          preferWeaponMatch{ true };
		bool          respectSexTags{ true };
		bool          notifications{ true };

		// [Stagger]
		bool  staggerEnabled{ true };
		float staggerInterval{ 2.5f };
		float staggerMagnitude{ 0.8f };
		float staggerWindow{ 10.0f };

		// [Marker]
		bool        markerEnabled{ true };
		std::string markerText{ "\xE6\xAE\xBA" };  // 殺 (UTF-8)
		float       markerSize{ 28.0f };
		float       markerOffsetX{ 70.0f };
		float       markerOffsetY{ 0.0f };
		float       markerHeight{ 28.0f };
		int         markerR{ 255 }, markerG{ 110 }, markerB{ 110 };

		// [Debug]
		bool verbose{ false };
		bool cancelUnpaired{ true };  // stop the player's animation when the victim does not follow

	private:
		Settings() = default;
	};
}
