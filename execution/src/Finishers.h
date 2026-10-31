#pragma once

#include <nlohmann/json.hpp>

namespace ED
{
	enum class Category : int
	{
		kFront = 0,
		kBack,
		kSneak,
		kBleed,
		kAny,
		kTotal
	};

	enum WeaponClass : int
	{
		kUnarmed = 0,
		kOneHand,
		kTwoHand,
		kRanged,
		kOther,
		kWeaponClassCount
	};

	const char* CategoryName(Category a_cat);
	const char* WeaponClassName(int a_class);

	struct Finisher
	{
		std::string                    event;  // animation event as the game sends it (pa_...)
		std::string                    key;    // lower-case event, used for saving
		std::vector<RE::TESIdleForm*>  idles;  // every idle form that plays this event
		std::set<std::string>          sources;
		std::string                    chain;  // idle tree path of the first idle, for display
		std::string                    editorID;
		Category                       autoCategory{ Category::kFront };
		int                            userCategory{ -1 };  // -1 = automatic
		bool                           creature{ false };
		std::string                    raceWord;  // humanoid race the victim must be (falmer, draugr), empty = any
		bool                           killmove{ false };
		bool                           femaleAttacker{ false };
		bool                           maleAttacker{ false };
		bool                           hasWeaponTags{ false };
		std::array<bool, kWeaponClassCount> weaponOk{ true, true, true, true, true };
		// per weapon class (Unarmed/1H/2H/Ranged/Other): used at all, and relative chance (0 = only via Next/Test)
		std::array<bool, kWeaponClassCount> on{ true, true, true, true, true };
		std::array<int, kWeaponClassCount>  chance{ 100, 100, 100, 100, 100 };
		int                            success{ 0 };
		std::array<int, kWeaponClassCount> fails{};

		Category GetCategory() const { return userCategory >= 0 ? static_cast<Category>(userCategory) : autoCategory; }
	};

	class Catalog
	{
	public:
		static Catalog& Get()
		{
			static Catalog c;
			return c;
		}

		void Build();             // scan every loaded IDLE form (call on kDataLoaded)
		void BlockPlayerRandom();  // add "attacker is not the player" to killmove idles in the idle tree
		void LoadUser();
		void SaveUser();

		// presets: on/off, chance and category of every finisher
		std::vector<std::string> ListPresets() const;
		bool                     SavePreset(const std::string& a_name);
		bool                     LoadPreset(const std::string& a_name);
		bool                     DeletePreset(const std::string& a_name);

		std::vector<Finisher>& All() { return _list; }
		Finisher*              Find(const std::string& a_key);

	private:
		Catalog() = default;
		nlohmann::json ToJson(bool a_stats) const;
		void           FromJson(const nlohmann::json& a_j, bool a_stats);
		std::vector<Finisher> _list;
		bool                  _blocked{ false };
	};
}
