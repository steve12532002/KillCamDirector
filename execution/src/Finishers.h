#pragma once

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
		bool                           killmove{ false };
		bool                           femaleAttacker{ false };
		bool                           maleAttacker{ false };
		bool                           hasWeaponTags{ false };
		std::array<bool, kWeaponClassCount> weaponOk{ true, true, true, true, true };
		bool                           enabled{ true };
		int                            chance{ 100 };  // relative chance (weight) of being picked, 0 = never at random
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

		std::vector<Finisher>& All() { return _list; }
		Finisher*              Find(const std::string& a_key);

	private:
		Catalog() = default;
		std::vector<Finisher> _list;
		bool                  _blocked{ false };
	};
}
