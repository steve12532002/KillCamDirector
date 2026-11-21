#pragma once

namespace Zan
{
	enum Source : int
	{
		kVanilla = 0,
		kLowBlow,
		kThicc,
		kSourceCount
	};
	const char* SourceName(int a_source);

	// One killmove idle that sits in the game's idle tree
	struct Killmove
	{
		RE::TESIdleForm*      idle{ nullptr };
		RE::TESConditionItem* gate{ nullptr };  // our "not the player" item: closed unless this one was picked
		std::string           event;
		std::string           editorID;
		std::string           file;
		int                   source{ kVanilla };
		RE::TESIdleForm*      top{ nullptr };   // highest node of its killmove branch (child of an attack-tree node)
		int                   attack{ 0 };  // AttackKind: the attack whose branch of the tree leads to it
		int                   started{ 0 };
		int                   missed{ 0 };
	};

	enum AttackKind : int
	{
		kRight = 0,
		kRightPower,
		kLeft,
		kLeftPower,
		kDualPower,
		kAttackKindCount
	};

	struct MarkerDraw
	{
		float x;
		float y;
	};

	class Core
	{
	public:
		static Core& Get()
		{
			static Core c;
			return c;
		}

		static void InstallHooks();

		void Build();  // after data load: find the killmoves, put the gates on them

		void OnKey();  // execute key pressed
		void Tick(RE::PlayerCharacter* a_player, float a_dt);
		void UpdateMarkerScreen();
		void OnPreLoad();

		std::vector<MarkerDraw> Markers()
		{
			std::scoped_lock l(_markerLock);
			return _markers;
		}

		const std::vector<Killmove>& All() const { return _all; }
		std::array<int, kSourceCount> CountBySource() const;
		const std::string&            LastResult() const { return _lastResult; }

	private:
		struct Mark
		{
			RE::ActorHandle handle;
		};

		struct Pending
		{
			bool            active{ false };
			RE::ActorHandle victim;
			Killmove*       pick{ nullptr };     // the light-attack pick (the one Zan's own swing reaches)
			std::vector<Killmove*> picks;        // one per attack kind: light, heavy, left... all open at once
			float           hp{ 0.0f };
			float           timer{ 0.0f };
			bool            started{ false };
			float           endTimer{ 0.0f };
			bool            sent{ false };    // the attack was accepted
			float           retry{ 0.0f };
			float           sentAt{ -1.0f };
			int             tries{ 0 };          // attacks Zan sent that the game did not take
			bool            idleSeen{ false };   // the player was between attacks at some point since the key  // timer value when the attack was accepted
		};

		Core() = default;

		bool        IsExecutable(RE::PlayerCharacter* a_player, RE::Actor* a_actor) const;
		RE::Actor*  PickVictim(RE::PlayerCharacter* a_player);
		Killmove*   Choose(RE::PlayerCharacter* a_player, RE::Actor* a_victim, std::initializer_list<int> a_kinds, bool a_log);
		void        Open(Killmove* a_km, bool a_open);
		void        Finish(bool a_started);
		void        UpdateMarks(RE::PlayerCharacter* a_player);
		bool        SendAttack(RE::PlayerCharacter* a_player, RE::Actor* a_victim, int a_kind);
		void        Dump(Killmove* a_km, RE::PlayerCharacter* a_player, RE::Actor* a_victim);

		std::vector<Killmove>                         _all;
		std::unordered_set<RE::TESIdleForm*>          _attackNodes;  // tree nodes that also lead to non-killmove idles
		std::unordered_map<RE::FormID, Mark>          _marked;
		float                                         _scan{ 0.0f };
		float                                         _clock{ 0.0f };
		Pending                                       _pending;
		// random gates made always-true while a killmove is open: (item, its original data)
		std::vector<std::pair<RE::TESConditionItem*, std::array<std::byte, sizeof(RE::CONDITION_ITEM_DATA)>>> _saved;
		std::deque<std::string>                       _recent;
		std::mt19937                                  _rng{ std::random_device{}() };
		std::string                                   _lastResult;
		std::mutex                                    _markerLock;
		std::vector<MarkerDraw>                       _markers;
		int                                           _dumps{ 0 };
		std::vector<RE::TESIdleForm*>                 _hosts;  // attack-tree nodes that hold killmove branches
		struct SavedArray
		{
			void*         array;
			RE::TESForm** data;
			std::uint32_t capacity, freeIdx, size;
		};
		std::vector<SavedArray>                       _savedArrays;
		std::array<int, kAttackKindCount>             _kindStarted{};
		std::array<int, kAttackKindCount>             _kindMissed{};
	};
}
