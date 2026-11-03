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

	// same split as VioLens' Customize Killmoves pages
	enum WeaponClass : int
	{
		kUnarmed = 0,
		kDagger,
		kSword,
		kAxe,
		kMace,
		kDualWield,
		kGreatsword,
		kBattleaxe,
		kWarhammer,
		kRanged,
		kOther,
		kWeaponClassCount
	};
	inline bool IsOneHanded(int a_wc) { return a_wc >= kDagger && a_wc <= kDualWield; }
	inline bool IsTwoHanded(int a_wc) { return a_wc >= kGreatsword && a_wc <= kWarhammer; }

	const char* CategoryName(Category a_cat);
	const char* WeaponClassName(int a_class);

	inline std::array<bool, kWeaponClassCount> AllTrue()
	{
		std::array<bool, kWeaponClassCount> a;
		a.fill(true);
		return a;
	}
	inline std::array<int, kWeaponClassCount> AllHundred()
	{
		std::array<int, kWeaponClassCount> a;
		a.fill(100);
		return a;
	}

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
		bool                           targetSpecific{ false };  // idle tree checks the victim's race
		std::string                    raceWord;  // humanoid race the victim must be (falmer, draugr), empty = any
		bool                           killmove{ false };
		bool                           femaleAttacker{ false };
		bool                           maleAttacker{ false };
		bool                           hasWeaponTags{ false };
		std::array<bool, kWeaponClassCount> weaponOk = AllTrue();
		// per weapon class (Unarmed/1H/2H/Ranged/Other): used at all, and relative chance (0 = only via Next/Test)
		std::array<bool, kWeaponClassCount> on = AllTrue();
		std::array<int, kWeaponClassCount>  chance = AllHundred();
		int                            success{ 0 };
		std::array<int, kWeaponClassCount> fails{};

		// creatures and special races (draugr, falmer, riekling...) are kept apart from the human finishers
		// (race checks in the idle tree are not used here: VioLens lists the human races in them, so they cannot tell
		// a human finisher from a creature one - the per-victim condition check sorts that out at execution time)
		bool IsCreatureGroup() const { return creature || !raceWord.empty(); }

		// failed repeatedly with this weapon and never proven to work often enough: left out of random picks
		bool AutoExcluded(int a_wc) const { return fails[a_wc] >= 3 && fails[a_wc] > success * 2; }

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
		void BlockPlayerRandom();
		void AttachOrphans();  // hang stand-alone killmove idles under the vanilla killmove root  // add "attacker is not the player" to killmove idles in the idle tree
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
