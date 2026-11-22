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
		void ForceTest();  // settings-page button: force a killmove on the nearest enemy (any health)
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
		const std::vector<Killmove*>& LastCandidates() const { return _lastCandidates; }
		bool                          IsBlocked(const Killmove* a_km) const { return _blocked.contains(a_km->editorID); }
		void                          Block(Killmove* a_km);
		void                          ClearBlocked();
		std::size_t                   BlockedCount() const { return _blocked.size(); }

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
			bool            idleSeen{ false };
			float           lonely{ 0.0f };
			bool            refusedIdle{ false };
			int             click{ 0 };          // 0 = not yet, 1 = button down, 2 = done
			float           clickTimer{ 0.0f };  // last swing was refused with the enemy not attacking      // time the player has been in the pair without the enemy   // the player was between attacks at some point since the key  // timer value when the attack was accepted
		};

		Core() = default;

		bool        IsExecutable(RE::PlayerCharacter* a_player, RE::Actor* a_actor) const;
		RE::Actor*  PickVictim(RE::PlayerCharacter* a_player);
		Killmove*   Choose(RE::PlayerCharacter* a_player, RE::Actor* a_victim, std::initializer_list<int> a_kinds, bool a_log);
		void        Open(Killmove* a_km, bool a_open);
		void        Finish(bool a_started);
		void        UpdateMarks(RE::PlayerCharacter* a_player);
		void        SwapLight(RE::PlayerCharacter* a_player, RE::Actor* a_victim);
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
		std::vector<Killmove*>                        _lastCandidates;  // the animation(s) that just played
		std::unordered_set<std::string>               _blocked;
		bool                                          _swingStarted{ false };
		RE::TESGlobal*                                _vlChance{ nullptr };
		RE::TESGlobal*                                _vlLastEnemy{ nullptr };
		float                                         _vlChanceOld{ 0.0f }, _vlLastEnemyOld{ 0.0f };
		bool                                          _vlSet{ false };
		std::vector<RE::TESConditionItem*>            _killChecks;  // "ShouldAttackKill" items on the killmove branches
		std::vector<std::pair<RE::TESConditionItem*, std::array<std::byte, sizeof(RE::CONDITION_ITEM_DATA)>>> _killSaved;
		void                                          VioLensGlobals(bool a_set);
	public:
		void                                          ApplyMode();  // opens / closes the gates for the pick mode
	private:
		// ---- direct play (pick mode 2): the key plays a paired killmove at once, no attack needed
		struct Force
		{
			bool            active{ false };
			RE::ActorHandle victim;
			Killmove*       km{ nullptr };
			float           timer{ 0.0f };
			bool            playerIn{ false };
			bool            victimIn{ false };
			float           endTimer{ 0.0f };
		};
		Force                                         _force;
		std::unordered_map<RE::TESIdleForm*, RE::TESIdleForm*> _twins;
		RE::TESIdleForm*                              Twin(RE::TESIdleForm* a_idle);
		bool                                          ForcePlay(RE::PlayerCharacter* a_player, RE::Actor* a_victim, bool a_anyHealth);
		void                                          ForceTick(RE::PlayerCharacter* a_player, float a_dt);
		std::vector<Killmove*>                        ForceOrder(RE::PlayerCharacter* a_player, RE::Actor* a_victim, int a_weaponKey);
		static int                                    WeaponKey(RE::PlayerCharacter* a_player);
		// what the game accepted / refused to play, per weapon in hand and animation (learned while playing)
		std::map<std::pair<int, std::string>, std::pair<int, int>> _learn;
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
