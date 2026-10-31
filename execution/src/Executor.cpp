#include "PCH.h"
#include "Executor.h"

#include "Settings.h"

#include <MinHook.h>

namespace ED
{
	namespace
	{
		using UpdateFn = void (*)(RE::PlayerCamera*);
		UpdateFn g_origUpdate = nullptr;

		void HookedUpdate(RE::PlayerCamera* a_this)
		{
			g_origUpdate(a_this);
			auto player = RE::PlayerCharacter::GetSingleton();
			auto ui = RE::UI::GetSingleton();
			if (!player || (ui && ui->GameIsPaused())) {
				return;
			}
			auto  timer = RE::BSTimer::GetSingleton();
			float dt = timer ? timer->realTimeDelta : (1.0f / 60.0f);
			Executor::Get().Tick(player, std::clamp(dt, 0.0f, 0.1f));
		}

		bool IsSynced(RE::Actor* a_actor)
		{
			static const RE::BSFixedString kSynced("bIsSynced");
			bool v = false;
			return a_actor && a_actor->GetGraphVariableBool(kSynced, v) && v;
		}

		float HealthPct(RE::Actor* a_actor)
		{
			auto av = a_actor->AsActorValueOwner();
			if (!av) {
				return 1.0f;
			}
			float cur = av->GetActorValue(RE::ActorValue::kHealth);
			float max = av->GetPermanentActorValue(RE::ActorValue::kHealth) +
			            a_actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kHealth);
			return max > 0.0f ? cur / max : 1.0f;
		}

		// Temporarily removes the conditions of an idle and its parents so a forced play is not
		// refused because of weapon checks, chance rolls or our own "not the player" block.
		class ConditionStrip
		{
		public:
			explicit ConditionStrip(RE::TESIdleForm* a_idle)
			{
				int guard = 0;
				for (auto i = a_idle; i && guard < 32; i = i->parentIdle, ++guard) {
					_saved.emplace_back(i, i->conditions.head);
					i->conditions.head = nullptr;
				}
			}
			~ConditionStrip()
			{
				for (auto& [idle, head] : _saved) {
					idle->conditions.head = head;
				}
			}

		private:
			std::vector<std::pair<RE::TESIdleForm*, RE::TESConditionItem*>> _saved;
		};
	}

	void Executor::InstallHooks()
	{
		REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(49852, 50784) };
		MH_STATUS st = MH_Initialize();
		if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
			logger::error("MinHook init failed");
			return;
		}
		auto addr = reinterpret_cast<LPVOID>(target.address());
		if (MH_CreateHook(addr, reinterpret_cast<LPVOID>(&HookedUpdate), reinterpret_cast<LPVOID*>(&g_origUpdate)) != MH_OK ||
			MH_EnableHook(addr) != MH_OK) {
			logger::error("Failed to hook the per-frame update");
			return;
		}
		logger::info("Per-frame hook installed");
	}

	void Executor::Notify(const char* a_msg)
	{
		if (!Settings::Get().notifications) {
			return;
		}
		using func_t = void (*)(const char*, const char*, bool);
		static REL::Relocation<func_t> func{ RELOCATION_ID(52050, 52933) };
		func(a_msg, nullptr, true);
	}

	bool Executor::IsExecutable(RE::PlayerCharacter* a_player, RE::Actor* a) const
	{
		auto& s = Settings::Get();
		if (!a || a == a_player || a->IsDead() || a->IsDisabled() || !a->Get3D(false)) {
			return false;
		}
		if (!a->IsHostileToActor(a_player)) {
			return false;
		}
		if (s.humanoidOnly && !a->HasKeywordString("ActorTypeNPC")) {
			return false;
		}
		if (!s.allowEssential && (a->IsEssential() || a->IsProtected())) {
			return false;
		}
		if (a->IsInKillMove() || IsSynced(a)) {
			return false;
		}
		if (a->GetPosition().GetDistance(a_player->GetPosition()) > s.markRange) {
			return false;
		}
		return HealthPct(a) <= s.healthThreshold;
	}

	void Executor::UpdateMarks(RE::PlayerCharacter* a_player)
	{
		std::unordered_set<RE::FormID> seen;
		if (auto lists = RE::ProcessLists::GetSingleton()) {
			lists->ForEachHighActor([&](RE::Actor* a) {
				if (IsExecutable(a_player, a)) {
					seen.insert(a->GetFormID());
					auto [it, inserted] = _marked.try_emplace(a->GetFormID());
					if (inserted) {
						it->second.handle = a->GetHandle();
						it->second.markedAt = _clock;
						if (Settings::Get().verbose) {
							logger::info("Executable: {}", a->GetDisplayFullName());
						}
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}
		for (auto it = _marked.begin(); it != _marked.end();) {
			if (!seen.contains(it->first)) {
				it = _marked.erase(it);
			} else {
				++it;
			}
		}
	}

	void Executor::UpdateStagger(RE::PlayerCharacter* a_player)
	{
		auto& s = Settings::Get();
		if (!s.staggerEnabled) {
			return;
		}
		static const RE::BSFixedString kMag("staggerMagnitude");
		static const RE::BSFixedString kStart("staggerStart");
		auto busyVictim = _attempt.active ? _attempt.victim.get() : RE::NiPointer<RE::Actor>{};
		for (auto& [id, m] : _marked) {
			auto a = m.handle.get();
			if (!a || (busyVictim && a.get() == busyVictim.get())) {
				continue;
			}
			if (_clock - m.markedAt > s.staggerWindow) {
				continue;
			}
			if (_clock - m.lastStagger < s.staggerInterval) {
				continue;
			}
			if (a->GetPosition().GetDistance(a_player->GetPosition()) > 1500.0f) {
				continue;
			}
			a->SetGraphVariableFloat(kMag, s.staggerMagnitude);
			a->NotifyAnimationGraph(kStart);
			m.lastStagger = _clock;
		}
	}

	void Executor::UpdateMarkerScreen()
	{
		std::vector<MarkerDraw> out;
		auto&                   s = Settings::Get();
		auto                    cam = RE::Main::WorldRootCamera();
		if (s.markerEnabled && cam) {
			static const RE::BSFixedString kHead("NPC Head [Head]");
			auto&                          rt = cam->GetRuntimeData();
			auto&                          rt2 = cam->GetRuntimeData2();
			for (auto& [id, m] : _marked) {
				auto a = m.handle.get();
				if (!a) {
					continue;
				}
				RE::NiPoint3 p = a->GetPosition();
				p.z += 120.0f;
				if (auto root = a->Get3D(false)) {
					if (auto head = root->GetObjectByName(kHead)) {
						p = head->world.translate;
					}
				}
				p.z += s.markerHeight;
				float x = 0, y = 0, z = 0;
				if (RE::NiCamera::WorldPtToScreenPt3(rt.worldToCam, rt2.port, p, x, y, z, 1e-5f) && z > 0.0f &&
					x > -0.1f && x < 1.1f && y > -0.1f && y < 1.1f) {
					out.push_back({ x, 1.0f - y });
				}
			}
		}
		std::scoped_lock l(_markerLock);
		_markers = std::move(out);
	}

	int Executor::WeaponClassOf(RE::PlayerCharacter* a_player) const
	{
		auto form = a_player->GetEquippedObject(false);
		auto weap = form ? form->As<RE::TESObjectWEAP>() : nullptr;
		if (!weap) {
			return kUnarmed;
		}
		switch (weap->GetWeaponType()) {
		case RE::WEAPON_TYPE::kHandToHandMelee:
			return kUnarmed;
		case RE::WEAPON_TYPE::kOneHandSword:
		case RE::WEAPON_TYPE::kOneHandDagger:
		case RE::WEAPON_TYPE::kOneHandAxe:
		case RE::WEAPON_TYPE::kOneHandMace:
			return kOneHand;
		case RE::WEAPON_TYPE::kTwoHandSword:
		case RE::WEAPON_TYPE::kTwoHandAxe:
			return kTwoHand;
		case RE::WEAPON_TYPE::kBow:
		case RE::WEAPON_TYPE::kCrossbow:
			return kRanged;
		default:
			return kOther;
		}
	}

	Category Executor::CategoryFor(RE::PlayerCharacter* a_player, RE::Actor* a_victim) const
	{
		if (auto st = a_victim->AsActorState(); st && st->IsBleedingOut()) {
			return Category::kBleed;
		}
		if (a_player->IsSneaking() && a_victim->RequestDetectionLevel(a_player) <= 0) {
			return Category::kSneak;
		}
		float ang = std::abs(a_victim->GetHeadingAngle(a_player->GetPosition(), true));
		return ang > 110.0f ? Category::kBack : Category::kFront;
	}

	void Executor::StartExecution(RE::PlayerCharacter* a_player)
	{
		auto& s = Settings::Get();
		if (_attempt.active || a_player->IsInKillMove() || IsSynced(a_player)) {
			return;
		}
		// nearest marked enemy in front of you and within reach
		RE::Actor* victim = nullptr;
		float      best = s.executeRange;
		for (auto& [id, m] : _marked) {
			auto a = m.handle.get();
			if (!a) {
				continue;
			}
			float d = a->GetPosition().GetDistance(a_player->GetPosition());
			float ang = std::abs(a_player->GetHeadingAngle(a->GetPosition(), true));
			if (d <= best && ang <= s.executeCone) {
				best = d;
				victim = a.get();
			}
		}
		if (!victim) {
			Notify("\xE9\x99\x84\xE8\xBF\x91\xE6\xB2\x92\xE6\x9C\x89\xE5\x8F\xAF\xE8\x99\x95\xE6\xB1\xBA\xE7\x9A\x84\xE6\x95\xB5\xE4\xBA\xBA");  // 附近沒有可處決的敵人
			return;
		}

		const Category cat = CategoryFor(a_player, victim);
		const int      wc = WeaponClassOf(a_player);
		const bool     female = a_player->GetActorBase() && a_player->GetActorBase()->GetSex() == RE::SEX::kFemale;
		const bool     humanVictim = victim->HasKeywordString("ActorTypeNPC");

		auto& all = Catalog::Get().All();
		auto  collect = [&](Category c) {
            std::vector<Finisher*> v;
            for (auto& f : all) {
                if (!f.enabled || !f.killmove || f.idles.empty()) continue;
                if (humanVictim && f.creature) continue;
                auto fc = f.GetCategory();
                if (fc != c && fc != Category::kAny) continue;
                if (s.respectSexTags && ((f.femaleAttacker && !female) || (f.maleAttacker && female))) continue;
                v.push_back(&f);
            }
            return v;
		};
		std::vector<Finisher*> cands = collect(cat);
		if (cands.empty() && cat == Category::kSneak) cands = collect(Category::kBack);
		if (cands.empty() && cat != Category::kFront) cands = collect(Category::kFront);
		if (cands.empty()) {
			Notify("\xE6\xB2\x92\xE6\x9C\x89\xE5\x8F\xAF\xE7\x94\xA8\xE7\x9A\x84\xE7\xB5\x82\xE7\xB5\x90\xE6\x8A\x80");  // 沒有可用的終結技
			return;
		}
		if (s.preferWeaponMatch) {
			std::vector<Finisher*> matched;
			for (auto f : cands) {
				if (f->weaponOk[wc] && f->fails[wc] < 3) {
					matched.push_back(f);
				}
			}
			if (!matched.empty()) {
				cands = std::move(matched);
			}
		}
		// chance 0 = only when queued with Next (unless nothing else is left)
		{
			std::vector<Finisher*> allowed;
			for (auto f : cands) {
				if (f->chance > 0) {
					allowed.push_back(f);
				}
			}
			if (!allowed.empty()) {
				cands = std::move(allowed);
			}
		}
		// weighted random order: key = u^(1/w), highest first (Efraimidis-Spirakis)
		{
			std::uniform_real_distribution<double> u01(1e-9, 1.0);
			std::vector<std::pair<double, Finisher*>> keyed;
			for (auto f : cands) {
				double w = std::max(1, f->chance) / 100.0;
				keyed.emplace_back(std::pow(u01(_rng), 1.0 / w), f);
			}
			std::sort(keyed.begin(), keyed.end(), [](auto& a, auto& b) { return a.first > b.first; });
			cands.clear();
			for (auto& [k, f] : keyed) {
				cands.push_back(f);
			}
		}
		// ones that have never failed with this weapon first
		std::stable_sort(cands.begin(), cands.end(), [wc](Finisher* a, Finisher* b) { return a->fails[wc] < b->fails[wc]; });
		if (!_forcedNext.empty()) {
			if (auto f = Catalog::Get().Find(_forcedNext)) {
				cands.insert(cands.begin(), f);
			}
			_forcedNext.clear();
		}

		_attempt = Attempt{};
		_attempt.active = true;
		_attempt.victim = victim->GetHandle();
		_attempt.candidates = std::move(cands);
		_attempt.weaponClass = wc;
		logger::info("Execute {} ({}, {} weapon): {} candidates", victim->GetDisplayFullName(), CategoryName(cat), WeaponClassName(wc), _attempt.candidates.size());
		if (!TryNext(a_player)) {
			_attempt.active = false;
		}
	}

	bool Executor::TryNext(RE::PlayerCharacter* a_player)
	{
		auto& s = Settings::Get();
		auto  victim = _attempt.victim.get();
		auto  process = a_player->GetActorRuntimeData().currentProcess;
		if (!victim || victim->IsDead() || !process) {
			return false;
		}
		while (_attempt.index < _attempt.candidates.size() && _attempt.tries < std::max(1, s.maxRetries)) {
			auto f = _attempt.candidates[_attempt.index++];
			++_attempt.tries;
			auto idle = f->idles.front();
			bool ok = false;
			{
				ConditionStrip strip(idle);
				ok = process->PlayIdle(a_player, idle, victim.get());
			}
			_attempt.current = f;
			_attempt.timer = 0;
			_attempt.started = false;
			logger::info("  try {} ({}) -> {}", f->event, idle->GetFormEditorID(), ok ? "sent" : "refused");
			if (ok) {
				return true;
			}
			f->fails[_attempt.weaponClass]++;
		}
		return false;
	}

	void Executor::UpdateAttempt(RE::PlayerCharacter* a_player, float a_dt)
	{
		if (!_attempt.active) {
			return;
		}
		auto& s = Settings::Get();
		bool  inPair = a_player->IsInKillMove() || IsSynced(a_player);
		if (!_attempt.started) {
			_attempt.timer += a_dt;
			if (inPair) {
				_attempt.started = true;
				if (_attempt.current) {
					_attempt.current->success++;
					_lastResult = _attempt.current->event;
				}
				return;
			}
			if (_attempt.timer > 0.5f) {
				if (_attempt.current) {
					_attempt.current->fails[_attempt.weaponClass]++;
					logger::info("  {} did not start", _attempt.current->event);
				}
				if (!TryNext(a_player)) {
					_attempt.active = false;
					_lastResult = "failed";
					Catalog::Get().SaveUser();
					Notify("\xE8\x99\x95\xE6\xB1\xBA\xE5\x8B\x95\xE7\x95\xAB\xE5\x95\x9F\xE5\x8B\x95\xE5\xA4\xB1\xE6\x95\x97");  // 處決動畫啟動失敗
				}
			}
			return;
		}
		// running: wait for the pair to finish
		if (inPair) {
			_attempt.endTimer = 0;
			return;
		}
		_attempt.endTimer += a_dt;
		if (_attempt.endTimer < 0.4f) {
			return;
		}
		if (auto victim = _attempt.victim.get(); victim && s.ensureKill && !victim->IsDead() &&
		                                         (s.allowEssential || !victim->IsEssential())) {
			logger::info("  victim survived the animation - finishing them");
			victim->KillImmediate();
		}
		_attempt.active = false;
		Catalog::Get().SaveUser();
	}

	void Executor::Tick(RE::PlayerCharacter* a_player, float a_dt)
	{
		auto& s = Settings::Get();
		_clock += a_dt;
		if (!s.enabled) {
			_marked.clear();
			UpdateMarkerScreen();
			return;
		}
		_scanTimer -= a_dt;
		if (_scanTimer <= 0.0f) {
			_scanTimer = 0.2f;
			UpdateMarks(a_player);
		}
		UpdateStagger(a_player);
		if (_executeRequested.exchange(false)) {
			StartExecution(a_player);
		}
		UpdateAttempt(a_player, a_dt);
		UpdateMarkerScreen();
	}
}
