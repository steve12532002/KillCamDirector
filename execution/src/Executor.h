#pragma once

#include "Finishers.h"

namespace ED
{
	struct MarkerDraw
	{
		float x;  // 0..1 screen space, left to right
		float y;  // 0..1 screen space, top to bottom
	};

	class Executor
	{
	public:
		static Executor& Get()
		{
			static Executor e;
			return e;
		}

		static void InstallHooks();
		static void Notify(const char* a_msg);

		void Tick(RE::PlayerCharacter* a_player, float a_dt);
		void RequestExecute() { _executeRequested = true; }
		void ForceNext(const std::string& a_key) { _forcedNext = a_key; }
		const std::string& ForcedNext() const { return _forcedNext; }

		std::vector<MarkerDraw> Markers()
		{
			std::scoped_lock l(_markerLock);
			return _markers;
		}
		std::size_t MarkedCount() const { return _marked.size(); }
		const std::string& LastResult() const { return _lastResult; }

	private:
		struct Mark
		{
			RE::ActorHandle handle;
			float           markedAt{ 0 };
			float           lastStagger{ -100 };
		};

		struct Attempt
		{
			bool                   active{ false };
			RE::ActorHandle        victim;
			std::vector<Finisher*> candidates;
			std::size_t            index{ 0 };
			int                    tries{ 0 };
			Finisher*              current{ nullptr };
			float                  timer{ 0 };
			bool                   started{ false };
			float                  endTimer{ 0 };
			int                    weaponClass{ 0 };
		};

		void        UpdateMarks(RE::PlayerCharacter* a_player);
		void        UpdateStagger(RE::PlayerCharacter* a_player);
		void        UpdateMarkerScreen();
		void        StartExecution(RE::PlayerCharacter* a_player);
		void        UpdateAttempt(RE::PlayerCharacter* a_player, float a_dt);
		bool        TryNext(RE::PlayerCharacter* a_player);
		bool        IsExecutable(RE::PlayerCharacter* a_player, RE::Actor* a_actor) const;
		int         WeaponClassOf(RE::PlayerCharacter* a_player) const;
		Category    CategoryFor(RE::PlayerCharacter* a_player, RE::Actor* a_victim) const;

		float                                       _clock{ 0 };
		float                                       _scanTimer{ 0 };
		std::unordered_map<RE::FormID, Mark>        _marked;
		std::mutex                                  _markerLock;
		std::vector<MarkerDraw>                     _markers;
		std::atomic<bool>                           _executeRequested{ false };
		Attempt                                     _attempt;
		std::string                                 _forcedNext;
		std::string                                 _lastResult;
		std::mt19937                                _rng{ std::random_device{}() };
	};
}
