#include "PCH.h"
#include "Executor.h"

#include "Settings.h"

#include <MinHook.h>

#ifdef GetObject
#	undef GetObject  // windows.h (UNICODE) renames it to GetObjectW
#endif

namespace ED
{
	namespace
	{
		using UpdateFn = void (*)(RE::PlayerCamera*);
		UpdateFn g_origUpdate = nullptr;

		// camera update: only the marker positions (needs this frame's camera)
		void HookedUpdate(RE::PlayerCamera* a_this)
		{
			g_origUpdate(a_this);
			auto ui = RE::UI::GetSingleton();
			if (ui && ui->GameIsPaused()) {
				return;
			}
			Executor::Get().UpdateMarkerScreen();
		}

		// Main::Update: all game logic. Starting idles from the camera update (as before) happens at a point of the
		// frame where the AI often refuses them; Kaputt drives its killmoves from here.
		struct MainUpdateHook
		{
			static void thunk(RE::Main* a_this, float a_arg)
			{
				func(a_this, a_arg);
				auto player = RE::PlayerCharacter::GetSingleton();
				auto ui = RE::UI::GetSingleton();
				if (!player || !player->Is3DLoaded() || (ui && ui->GameIsPaused())) {
					return;
				}
				static REL::Relocation<float*> deltaTime{ RELOCATION_ID(523661, 410200) };
				Executor::Get().Tick(player, std::clamp(*deltaTime, 0.0f, 0.1f));
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct HitHook
		{
			static void thunk(RE::Actor* a_victim, RE::HitData& a_hit)
			{
				Executor::Get().OnPlayerHit(a_victim, a_hit, false);
				func(a_victim, a_hit);
				Executor::Get().OnPlayerHit(a_victim, a_hit, true);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		float DamageMult()
		{
			static const char* kDiff[] = { "VE", "E", "N", "H", "VH", "L" };
			auto player = RE::PlayerCharacter::GetSingleton();
			int  d = player ? std::clamp(player->GetGameStatsData().difficulty, 0, 5) : 2;
			auto name = std::string("fDiffMultHPByPC") + kDiff[d];
			auto gs = RE::GameSettingCollection::GetSingleton();
			auto setting = gs ? gs->GetSetting(name.c_str()) : nullptr;
			return setting ? setting->data.f : 1.0f;
		}

		// in a paired animation: the GetPairedAnimation condition (what Kaputt uses) or the synced graph variable
		bool IsSynced(RE::Actor* a_actor)
		{
			if (!a_actor) {
				return false;
			}
			static RE::TESConditionItem cond;
			static std::once_flag       once;
			std::call_once(once, [] {
				cond.data.functionData.function = RE::FUNCTION_DATA::FunctionID::kGetPairedAnimation;
				cond.data.comparisonValue.f = 0.0f;
				cond.data.flags.opCode = RE::CONDITION_ITEM_DATA::OpCode::kNotEqualTo;
				cond.data.object = RE::CONDITIONITEMOBJECT::kSelf;
			});
			RE::ConditionCheckParams params(a_actor, nullptr);
			if (cond(params)) {
				return true;
			}
			static const RE::BSFixedString kSynced("bIsSynced");
			bool v = false;
			return a_actor->GetGraphVariableBool(kSynced, v) && v;
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

	namespace
	{
		std::string Lower(std::string a_s)
		{
			std::transform(a_s.begin(), a_s.end(), a_s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_s;
		}

		// conditions that only gate *random* selection, not whether the pair can work
		bool IgnoredCondition(const RE::TESConditionItem* a_item)
		{
			using F = RE::FUNCTION_DATA::FunctionID;
			auto fn = a_item->data.functionData.function.get();
			if (fn == F::kGetRandomPercent || fn == F::kGetGlobalValue) {
				return true;
			}
			// our own "not the player" block
			if (fn == F::kGetIsID && a_item->data.object == RE::CONDITIONITEMOBJECT::kSelf &&
				a_item->data.functionData.params[0] == RE::TESForm::LookupByID(0x7)) {
				return true;
			}
			return false;
		}

		// Evaluates an idle and all its parents with the player as subject and the victim as target.
		// OR items bind to their neighbours, AND separates groups - the same rule the game uses.
		bool IdleConditionsPass(RE::TESIdleForm* a_idle, RE::Actor* a_subject, RE::Actor* a_target, int& a_failFn)
		{
			int guard = 0;
			for (auto i = a_idle; i && guard < 32; i = i->parentIdle, ++guard) {
				bool groupOk = false;
				bool groupHasItems = false;
				for (auto item = i->conditions.head; item; item = item->next) {
					bool ok = true;
					if (!IgnoredCondition(item)) {
						RE::ConditionCheckParams params(a_subject, a_target);
						ok = item->IsTrue(params);
					}
					groupOk = groupOk || ok;
					groupHasItems = true;
					if (!item->data.flags.isOR) {
						if (!groupOk) {
							a_failFn = static_cast<int>(item->data.functionData.function.get());
							return false;
						}
						groupOk = false;
						groupHasItems = false;
					}
				}
				if (groupHasItems && !groupOk) {
					a_failFn = -1;
					return false;
				}
			}
			return true;
		}

		std::string RaceOf(RE::Actor* a_actor)
		{
			auto race = a_actor->GetRace();
			const char* id = race ? race->GetFormEditorID() : nullptr;
			return Lower(id && *id ? id : (race && race->GetName() ? race->GetName() : ""));
		}

		bool GraphBool(RE::Actor* a_actor, const RE::BSFixedString& a_name)
		{
			bool v = false;
			return a_actor->GetGraphVariableBool(a_name, v) && v;
		}

		void ClearKillMove(RE::Actor* a_actor)
		{
			if (a_actor) {
				a_actor->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);
			}
		}
	}

	void Executor::InstallHooks()
	{
		{
			REL::Relocation<std::uintptr_t> mainUpdate{ RELOCATION_ID(35551, 36544), REL::VariantOffset(0x11F, 0x160, 0x0) };
			SKSE::AllocTrampoline(64);
			MainUpdateHook::func = SKSE::GetTrampoline().write_call<5>(mainUpdate.address(), MainUpdateHook::thunk);
			logger::info("Main update hook installed");

		}
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
				// no longer executable (healed, out of range...): let them get up - unless they are being executed
				if (it->second.downed) {
					auto a = it->second.handle.get();
					if (a && !a->IsDead() && !IsVictim(a.get()) && !a->IsInKillMove() && !IsSynced(a.get())) {
						SetDowned(a.get(), false);
					}
				}
				it = _marked.erase(it);
			} else {
				++it;
			}
		}
	}

	bool Executor::IsVictim(RE::Actor* a_actor)
	{
		if (!_attempt.active) {
			return false;
		}
		auto v = _attempt.victim.get();
		return v && v.get() == a_actor;
	}

	void Executor::SetDowned(RE::Actor* a_actor, bool a_down)
	{
		static const RE::BSFixedString kStart("BleedoutStart");
		static const RE::BSFixedString kStop("BleedoutStop");
		auto st = a_actor->AsActorState();
		if (!st) {
			return;
		}
		auto& flags = a_actor->GetActorRuntimeData().boolFlags;
		if (a_down) {
			st->actorState1.lifeState = RE::ACTOR_LIFE_STATE::kBleedout;
			flags.set(RE::Actor::BOOL_FLAGS::kNoBleedoutRecovery);
			a_actor->NotifyAnimationGraph(kStart);
		} else {
			flags.reset(RE::Actor::BOOL_FLAGS::kNoBleedoutRecovery);
			if (st->actorState1.lifeState == RE::ACTOR_LIFE_STATE::kBleedout) {
				st->actorState1.lifeState = RE::ACTOR_LIFE_STATE::kAlive;
			}
			a_actor->NotifyAnimationGraph(kStop);
		}
	}

	void Executor::UpdateBleedout()
	{
		auto& s = Settings::Get();
		for (auto& [id, m] : _marked) {
			auto a = m.handle.get();
			if (!a || a->IsDead()) {
				continue;
			}
			bool busy = IsVictim(a.get()) || a->IsInKillMove() || IsSynced(a.get());
			if (!m.downed && !m.bleedDone && s.bleedoutEnabled && !busy) {
				if (auto st = a->AsActorState(); st && !st->IsBleedingOut()) {
					SetDowned(a.get(), true);
					if (s.verbose) {
						logger::info("{} kneels (bleedout)", a->GetDisplayFullName());
					}
				}
				m.downed = true;
				m.markedAt = _clock;
				continue;
			}
			if (!m.downed || busy) {
				continue;
			}
			if (_clock - m.markedAt >= s.bleedoutTime) {
				SetDowned(a.get(), false);
				m.downed = false;
				m.bleedDone = true;
				continue;
			}
			// the engine may try to let them recover on its own
			if (auto st = a->AsActorState(); st && st->actorState1.lifeState != RE::ACTOR_LIFE_STATE::kBleedout) {
				st->actorState1.lifeState = RE::ACTOR_LIFE_STATE::kBleedout;
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
			if (!a || (busyVictim && a.get() == busyVictim.get()) || m.downed) {
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
				RE::NiCamera::WorldPtToScreenPt3(rt.worldToCam, rt2.port, p, x, y, z, 1e-5f);
				// in front of the camera? (the sign of z differs between setups, so test it directly)
				auto       camPos = cam->world.translate;
				auto       player = RE::PlayerCharacter::GetSingleton();
				RE::NiPoint3 fwd = player ? player->GetPosition() - camPos : RE::NiPoint3{};
				fwd.z = 0.0f;
				RE::NiPoint3 to = p - camPos;
				to.z = 0.0f;
				bool front = fwd.Length() < 1.0f || fwd.Dot(to) > 0.0f;
				static int logged = 0;
				if (logged < 5) {
					++logged;
					logger::info("Marker for {}: screen x {:.3f} y {:.3f} z {:.3f}, in front {}", a->GetDisplayFullName(), x, y, z, front);
				}
				if (front && x > -0.1f && x < 1.1f && y > -0.1f && y < 1.1f) {
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

	bool Executor::BuildCandidates(RE::PlayerCharacter* a_player, RE::Actor* victim, std::vector<Finisher*>& a_out, int& wc, Category& cat)
	{
		auto& s = Settings::Get();
		cat = CategoryFor(a_player, victim);
		wc = WeaponClassOf(a_player);
		const bool     female = a_player->GetActorBase() && a_player->GetActorBase()->GetSex() == RE::SEX::kFemale;
		// a person: ActorTypeNPC and not one of the special races that have their own finishers
		const std::string victimRace = RaceOf(victim);
		const bool     humanVictim = victim->HasKeywordString("ActorTypeNPC") &&
		                         victimRace.find("draugr") == std::string::npos && victimRace.find("falmer") == std::string::npos;

		auto& all = Catalog::Get().All();
		bool  useAutoOff = true;
		auto  collect = [&](Category c) {
            std::vector<Finisher*> v;
            for (auto& f : all) {
                if (!f.on[wc] || !f.killmove || f.idles.empty()) continue;
                // the two groups never mix: people get human finishers, everything else creature / race finishers
                if (humanVictim == f.IsCreatureGroup()) continue;
                if (useAutoOff && f.AutoExcluded(wc)) continue;                  // kept failing with this weapon
                auto fc = f.GetCategory();
                if (fc != c && fc != Category::kAny) continue;
                if (s.respectSexTags && ((f.femaleAttacker && !female) || (f.maleAttacker && female))) continue;
                v.push_back(&f);
            }
            return v;
		};
		// a kneeling victim: bleedout finishers, plus the normal ones for where you stand (they pull the victim up)
		auto gather = [&]() {
			std::vector<Finisher*> v = collect(cat);
			if (cat == Category::kBleed) {
				float ang = std::abs(victim->GetHeadingAngle(a_player->GetPosition(), true));
				for (auto f : collect(ang > 110.0f ? Category::kBack : Category::kFront)) {
					if (std::find(v.begin(), v.end(), f) == v.end()) {
						v.push_back(f);
					}
				}
			}
			return v;
		};
		std::vector<Finisher*> cands = gather();
		if (cands.empty()) {
			useAutoOff = false;  // everything was auto-off: better to try them again than do nothing
			cands = gather();
		}
		if (cands.empty() && cat == Category::kSneak) cands = collect(Category::kBack);
		if (cands.empty() && cat != Category::kFront) cands = collect(Category::kFront);
		if (cands.empty()) {
			Notify("\xE6\xB2\x92\xE6\x9C\x89\xE5\x8F\xAF\xE7\x94\xA8\xE7\x9A\x84\xE7\xB5\x82\xE7\xB5\x90\xE6\x8A\x80");  // 沒有可用的終結技
			return false;
		}
		// Race checks are hard: an animation made for another race never pairs (the victim's graph ignores it).
		// Other idle tree conditions (weapon, angle...) are only a preference, since some of them never pass
		// outside a real killing blow.
		{
			const std::string      race = RaceOf(victim);
			std::vector<Finisher*> raceOk, valid;
			int                    logged = 0;
			for (auto f : cands) {
				if (!f->raceWord.empty() && race.find(f->raceWord) == std::string::npos) {
					continue;
				}
				int  failFn = 0;
				bool any = false;
				bool raceFail = true;
				for (auto idle : f->idles) {
					if (IdleConditionsPass(idle, a_player, victim, failFn)) {
						any = true;
						raceFail = false;
						break;
					}
					if (failFn != static_cast<int>(RE::FUNCTION_DATA::FunctionID::kGetIsRace) &&
						failFn != static_cast<int>(RE::FUNCTION_DATA::FunctionID::kGetIsID)) {
						raceFail = false;
					}
				}
				if (raceFail) {
					if (logged++ < 20) {
						logger::info("  skip {} (made for another race)", f->event);
					}
					continue;
				}
				raceOk.push_back(f);
				if (any) {
					valid.push_back(f);
				}
			}
			logger::info("  {} animations for this race, {} pass all of their conditions", raceOk.size(), valid.size());
			cands = !valid.empty() ? std::move(valid) : std::move(raceOk);
			if (cands.empty()) {
				Notify("\xE6\xB2\x92\xE6\x9C\x89\xE5\x8F\xAF\xE7\x94\xA8\xE7\x9A\x84\xE7\xB5\x82\xE7\xB5\x90\xE6\x8A\x80");  // 沒有可用的終結技
				return false;
			}
		}
		if (s.preferWeaponMatch) {
			std::vector<Finisher*> matched;
			for (auto f : cands) {
				if (f->weaponOk[wc]) {
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
				if (f->chance[wc] > 0) {
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
				double w = std::max(1, f->chance[wc]) / 100.0;
				// the last few animations played are much less likely, so it does not keep repeating
				if (std::find(_recent.begin(), _recent.end(), f) != _recent.end()) {
					w *= 0.15;
				}
				keyed.emplace_back(std::pow(u01(_rng), 1.0 / w), f);
			}
			std::sort(keyed.begin(), keyed.end(), [](auto& a, auto& b) { return a.first > b.first; });
			cands.clear();
			for (auto& [k, f] : keyed) {
				cands.push_back(f);
			}
		}
		if (!_forcedNext.empty()) {
			if (auto f = Catalog::Get().Find(_forcedNext)) {
				cands.insert(cands.begin(), f);
			}
			_forcedNext.clear();
		}

		a_out = std::move(cands);
		return !a_out.empty();
	}

	void Executor::StartExecution(RE::PlayerCharacter* a_player, RE::Actor* a_victim)
	{
		auto& s = Settings::Get();
		if (_attempt.active || a_player->IsInKillMove() || IsSynced(a_player)) {
			return;
		}
		// nearest marked enemy in front of you and within reach (or the one just hit)
		RE::Actor* victim = a_victim;
		float      best = s.executeRange;
		for (auto& [id, m] : _marked) {
			if (victim) {
				break;
			}
			auto a = m.handle.get();
			if (!a) {
				continue;
			}
			float d = a->GetPosition().GetDistance(a_player->GetPosition());
			float ang = std::abs(a_player->GetHeadingAngle(a->GetPosition(), true));
			// on slopes and stairs the pair ends up floating or sunk into the ground
			float dz = std::abs(a->GetPositionZ() - a_player->GetPositionZ());
			if (d <= best && ang <= s.executeCone && dz <= 45.0f) {
				best = d;
				victim = a.get();
			}
		}
		if (!victim) {
			Notify("\xE9\x99\x84\xE8\xBF\x91\xE6\xB2\x92\xE6\x9C\x89\xE5\x8F\xAF\xE8\x99\x95\xE6\xB1\xBA\xE7\x9A\x84\xE6\x95\xB5\xE4\xBA\xBA");  // 附近沒有可處決的敵人
			return;
		}

		std::vector<Finisher*> cands;
		int                    wc = 0;
		Category               cat = Category::kFront;
		if (!BuildCandidates(a_player, victim, cands, wc, cat)) {
			return;
		}
		logger::info("Execute {} ({}, {} weapon): {} candidates", victim->GetDisplayFullName(), CategoryName(cat), WeaponClassName(wc), cands.size());
		if (s.engineKillmoves && TryEngineKillmove(a_player, victim, cands, wc)) {
			return;
		}
		Begin(victim, std::move(cands), wc, false);
	}

	void Executor::StartTest(RE::PlayerCharacter* a_player, const std::string& a_key)
	{
		if (_attempt.active || a_player->IsInKillMove() || IsSynced(a_player)) {
			return;
		}
		auto f = Catalog::Get().Find(a_key);
		if (!f || f->idles.empty()) {
			return;
		}
		// nearest living actor, friend or foe, regardless of health
		RE::Actor* victim = nullptr;
		float      best = 800.0f;
		if (auto pl = RE::ProcessLists::GetSingleton()) {
			pl->ForEachHighActor([&](RE::Actor* a) {
				if (a && a != a_player && !a->IsDead() && !a->IsInKillMove() && !IsSynced(a) && a->Is3DLoaded()) {
					float d = a->GetPosition().GetDistance(a_player->GetPosition());
					if (d < best) {
						best = d;
						victim = a;
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}
		if (!victim) {
			Notify("\xE6\xB8\xAC\xE8\xA9\xA6\xEF\xBC\x9A\xE9\x99\x84\xE8\xBF\x91\xE6\xB2\x92\xE6\x9C\x89\xE7\x9B\xAE\xE6\xA8\x99");  // 測試：附近沒有目標
			return;
		}
		logger::info("Test {} on {}", f->event, victim->GetDisplayFullName());
		Begin(victim, { f }, WeaponClassOf(a_player), true);
	}

	bool Executor::VictimBusy(RE::Actor* a_victim)
	{
		static const RE::BSFixedString kAtk("IsAttacking");
		static const RE::BSFixedString kStag("IsStaggering");
		static const RE::BSFixedString kRec("IsRecoiling");
		static const RE::BSFixedString kBlk("IsBlocking");
		return GraphBool(a_victim, kAtk) || GraphBool(a_victim, kStag) || GraphBool(a_victim, kRec) || GraphBool(a_victim, kBlk);
	}

	namespace
	{
		// Temporarily unlinks the condition items that only gate random selection (chance, globals, our player
		// block) from an idle chain, keeping weapon / angle / race checks for the engine to evaluate.
		class RandomGateStrip
		{
		public:
			void Add(RE::TESIdleForm* a_idle)
			{
				int guard = 0;
				for (auto i = a_idle; i && guard < 32; i = i->parentIdle, ++guard) {
					if (std::find_if(_heads.begin(), _heads.end(), [&](auto& h) { return h.first == i; }) != _heads.end()) {
						continue;
					}
					_heads.emplace_back(i, i->conditions.head);
					RE::TESConditionItem*  newHead = nullptr;
					RE::TESConditionItem** link = &newHead;
					for (auto item = i->conditions.head; item; item = item->next) {
						_nexts.emplace_back(item, item->next);
					}
					for (auto& [item, next] : std::vector(_nexts.end() - CountItems(i->conditions.head), _nexts.end())) {
						if (!IgnoredCondition(item)) {
							*link = item;
							link = &item->next;
						}
					}
					*link = nullptr;
					i->conditions.head = newHead;
				}
			}
			~RandomGateStrip()
			{
				for (auto& [item, next] : _nexts) {
					item->next = next;
				}
				for (auto& [idle, head] : _heads) {
					idle->conditions.head = head;
				}
			}

		private:
			static std::ptrdiff_t CountItems(RE::TESConditionItem* a_head)
			{
				std::ptrdiff_t n = 0;
				for (auto i = a_head; i; i = i->next) {
					++n;
				}
				return n;
			}
			std::vector<std::pair<RE::TESIdleForm*, RE::TESConditionItem*>>     _heads;
			std::vector<std::pair<RE::TESConditionItem*, RE::TESConditionItem*>> _nexts;
		};

		struct AttackContext
		{
			RE::ActorHandle                  victim;
			std::vector<Finisher*>           chosen;
			std::unique_ptr<RandomGateStrip> strip;
			float                            hp{ 0 };
			int                              wc{ 0 };
		};
	}

	bool Executor::TryEngineKillmove(RE::PlayerCharacter* a_player, RE::Actor* a_victim, const std::vector<Finisher*>& a_cands, int a_wc)
	{
		// VioLens never plays a killmove itself: it only decides which idles are allowed and lets the game's own
		// killmove system pick one when an attack starts. That path always works, because the game starts the pair
		// from inside the attack where the transition exists. Do the same: allow the chosen finishers, make the
		// next blow lethal and start an attack.
		if (a_cands.empty() || !a_cands.front()->idles.front()->parentIdle) {
			return false;  // first pick is a stand-alone (Kaputt-style) idle: play it directly
		}
		std::vector<Finisher*> chosen;
		for (auto f : a_cands) {
			bool inTree = false, pass = false;
			for (auto idle : f->idles) {
				if (idle->parentIdle) {
					inTree = true;
					int fn = 0;
					pass = pass || IdleConditionsPass(idle, a_player, a_victim, fn);
				}
			}
			if (inTree && pass) {
				chosen.push_back(f);
			}
			if (chosen.size() >= 3) {
				break;
			}
		}
		if (chosen.empty()) {
			return false;
		}
		auto dom = RE::BGSDefaultObjectManager::GetSingleton();
		auto attack = dom ? dom->GetObject<RE::BGSAction>(RE::DEFAULT_OBJECT::kActionRightAttack) : nullptr;
		auto avo = a_victim->AsActorValueOwner();
		if (!attack || !avo) {
			return false;
		}

		PrepareVictim(a_victim);
		FaceEachOther(a_player, a_victim, chosen.front()->GetCategory());
		// the game only starts a killmove when the blow would kill
		float hp = avo->GetActorValue(RE::ActorValue::kHealth);
		if (hp > 1.0f) {
			avo->DamageActorValue(RE::ActorValue::kHealth, hp - 1.0f);
		}
		bool started = false;
		{
			RandomGateStrip strip;
			for (auto f : chosen) {
				for (auto idle : f->idles) {
					if (idle->parentIdle) {
						strip.Add(idle);
					}
				}
			}
			auto action = RE::TESActionData::Create();
			if (action) {
				action->source = RE::NiPointer<RE::TESObjectREFR>(a_player);
				action->target = RE::NiPointer<RE::TESObjectREFR>(a_victim);
				action->action = attack;
				action->Process();
				// not freed here: the game may still hold on to it while the attack runs
			}
			started = a_player->IsInKillMove() || IsSynced(a_player);
		}
		std::string names;
		for (auto f : chosen) {
			names += f->event + " ";
		}
		logger::info("  attack sent for a game killmove (allowed: {}){}", names, started ? " - started" : "");
		// the killmove may only begin a frame or two later: watch for it in UpdateAttempt
		_attempt = Attempt{};
		_attempt.active = true;
		_attempt.victim = a_victim->GetHandle();
		_attempt.weaponClass = a_wc;
		_attempt.prepped = true;
		_attempt.engine = true;
		_attempt.engineHp = hp;
		_attempt.candidates = a_cands;
		_attempt.current = chosen.front();  // best guess; the game picked one of these
		return true;
	}

	RE::Actor* Executor::PickVictim(RE::PlayerCharacter* a_player)
	{
		auto&      s = Settings::Get();
		RE::Actor* victim = nullptr;
		float      best = s.executeRange;
		for (auto& [id, m] : _marked) {
			auto a = m.handle.get();
			if (!a || a->IsDead()) {
				continue;
			}
			float d = a->GetPosition().GetDistance(a_player->GetPosition());
			float ang = std::abs(a_player->GetHeadingAngle(a->GetPosition(), true));
			float dz = std::abs(a->GetPositionZ() - a_player->GetPositionZ());
			if (d <= best && ang <= s.executeCone && dz <= 45.0f) {
				best = d;
				victim = a.get();
			}
		}
		return victim;
	}

	void Executor::Arm()
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		if (!player || _attempt.active || player->IsInKillMove() || IsSynced(player)) {
			return;
		}
		if (_armCtx) {
			_armedUntil = _clock + Settings::Get().armWindow;  // already armed: just extend
			return;
		}
		auto victim = PickVictim(player);
		if (!victim) {
			Notify("\xE9\x99\x84\xE8\xBF\x91\xE6\xB2\x92\xE6\x9C\x89\xE5\x8F\xAF\xE8\x99\x95\xE6\xB1\xBA\xE7\x9A\x84\xE6\x95\xB5\xE4\xBA\xBA");  // 附近沒有可處決的敵人
			return;
		}
		if (ArmOn(player, victim, Settings::Get().armWindow)) {
			Notify("\xE6\x94\xBB\xE6\x93\x8A\xE4\xBB\xA5\xE8\x99\x95\xE6\xB1\xBA");  // 攻擊以處決
		}
	}

	bool Executor::ArmOn(RE::PlayerCharacter* a_player, RE::Actor* a_victim, float a_window)
	{
		// Prepared *before* the attack, the way VioLens works: the chosen finishers have their chance gates lifted
		// and the victim is on 1 HP, so whatever attack the player starts next, the game's own killmove check
		// (wherever it runs) sees a lethal blow and is allowed exactly these finishers.
		std::vector<Finisher*> cands;
		int                    wc = 0;
		Category               cat = Category::kFront;
		if (!BuildCandidates(a_player, a_victim, cands, wc, cat)) {
			return false;
		}
		auto ctx = std::make_shared<AttackContext>();
		for (auto f : cands) {
			if (std::any_of(f->idles.begin(), f->idles.end(), [](auto i) { return i->parentIdle != nullptr; })) {
				ctx->chosen.push_back(f);
			}
			if (ctx->chosen.size() >= 4) {
				break;
			}
		}
		if (ctx->chosen.empty()) {
			logger::info("Arm on {}: none of the {} candidates is in the idle tree", a_victim->GetDisplayFullName(), cands.size());
			return false;
		}
		ctx->victim = a_victim->GetHandle();
		ctx->wc = wc;
		ctx->strip = std::make_unique<RandomGateStrip>();
		for (auto f : ctx->chosen) {
			for (auto idle : f->idles) {
				if (idle->parentIdle) {
					ctx->strip->Add(idle);
				}
			}
		}
		if (auto avo = a_victim->AsActorValueOwner()) {
			ctx->hp = avo->GetActorValue(RE::ActorValue::kHealth);
			if (ctx->hp > 1.0f) {
				avo->DamageActorValue(RE::ActorValue::kHealth, ctx->hp - 1.0f);
			}
		}
		std::string names;
		for (auto f : ctx->chosen) {
			names += f->event + " ";
		}
		logger::info("Armed on {} ({}, {} weapon) - allowed: {}", a_victim->GetDisplayFullName(), CategoryName(cat), WeaponClassName(wc), names);
		_armCtx = ctx;
		_armedUntil = _clock + a_window;
		return true;
	}

	void Executor::Disarm(bool a_started)
	{
		if (!_armCtx) {
			return;
		}
		auto ctx = std::static_pointer_cast<AttackContext>(_armCtx);
		_armCtx.reset();
		_armedUntil = -1.0f;
		auto victim = ctx->victim.get();
		ctx->strip.reset();  // conditions back as they were
		if (a_started) {
			logger::info("  game killmove started");
			_attempt = Attempt{};
			_attempt.active = true;
			_attempt.victim = ctx->victim;
			_attempt.weaponClass = ctx->wc;
			_attempt.prepped = true;
			_attempt.engine = true;
			_attempt.current = ctx->chosen.front();  // best guess: the game picked one of these
			return;
		}
		if (victim && !victim->IsDead() && ctx->hp > 1.0f) {
			if (auto avo = victim->AsActorValueOwner()) {
				avo->RestoreActorValue(RE::ActorValue::kHealth, ctx->hp - 1.0f);
			}
		}
		logger::info("  disarmed without a killmove");
	}

	void Executor::UpdateArm(RE::PlayerCharacter* a_player)
	{
		auto& s = Settings::Get();
		// automatic mode: arm on the marked enemy in front of you (one roll per enemy)
		if (!_armCtx && s.hitTrigger && !_attempt.active && !a_player->IsInKillMove()) {
			if (auto victim = PickVictim(a_player)) {
				auto it = _marked.find(victim->GetFormID());
				if (it != _marked.end() && !it->second.autoRolled) {
					it->second.autoRolled = true;
					std::uniform_real_distribution<float> roll(0.0f, 100.0f);
					if (roll(_rng) < s.hitChance) {
						ArmOn(a_player, victim, 1.0f);
					}
				}
			}
		}
		if (!_armCtx) {
			return;
		}
		auto ctx = std::static_pointer_cast<AttackContext>(_armCtx);
		auto victim = ctx->victim.get();
		// only the kill-move flag counts: a synced graph alone also happens for idles the attack picked up by mistake
		if (a_player->IsInKillMove()) {
			Disarm(true);
			return;
		}
		if (!victim || victim->IsDead()) {
			Disarm(false);
			return;
		}
		// automatic mode stays armed while that enemy stays in front of you
		if (s.hitTrigger && PickVictim(a_player) == victim.get()) {
			_armedUntil = std::max(_armedUntil, _clock + 0.5f);
		}
		if (_clock > _armedUntil) {
			Disarm(false);
		}
	}

	void Executor::FaceEachOther(RE::Actor* a_player, RE::Actor* a_victim, Category a_cat)
	{
		// Paired animations start from the attacker's current facing; the engine only pulls the victim into place.
		// Turn the attacker straight at the victim, and the victim towards (front) or away from (back) the attacker.
		auto  pp = a_player->GetPosition();
		auto  vp = a_victim->GetPosition();
		float toVictim = std::atan2(vp.x - pp.x, vp.y - pp.y);
		a_player->SetHeading(toVictim);
		bool  behind = a_cat == Category::kBack || a_cat == Category::kSneak;
		float victimHeading = behind ? toVictim : toVictim + RE::NI_PI;
		if (victimHeading > RE::NI_PI) {
			victimHeading -= RE::NI_TWO_PI;
		}
		a_victim->SetHeading(victimHeading);
	}

	void Executor::PrepareVictim(RE::Actor* a_victim)
	{
		// a paired animation is only accepted while the victim's graph is free: cut their attack, stagger or idle
		static const RE::BSFixedString kAttackStop("attackStop");
		static const RE::BSFixedString kStaggerStop("staggerStop");
		static const RE::BSFixedString kDefault("IdleForceDefaultState");
		static const RE::BSFixedString kBlockStop("blockStop");
		static const RE::BSFixedString kGetUpEnd("GetUpEnd");
		// the attacker must be free as well (Kaputt stops both sides' attacks and staggers)
		if (auto player = RE::PlayerCharacter::GetSingleton()) {
			player->NotifyAnimationGraph(kAttackStop);
			player->NotifyAnimationGraph(kStaggerStop);
		}
		if (auto st = a_victim->AsActorState();
			st && (st->GetKnockState() == RE::KNOCK_STATE_ENUM::kGetUp || st->GetKnockState() == RE::KNOCK_STATE_ENUM::kQueued)) {
			st->actorState1.knockState = RE::KNOCK_STATE_ENUM::kNormal;
			a_victim->NotifyAnimationGraph(kGetUpEnd);
		}
		a_victim->NotifyAnimationGraph(kAttackStop);
		a_victim->NotifyAnimationGraph(kStaggerStop);
		a_victim->NotifyAnimationGraph(kBlockStop);
	}

	void Executor::Begin(RE::Actor* a_victim, std::vector<Finisher*> a_cands, int a_wc, bool a_test)
	{
		_attempt = Attempt{};
		_attempt.active = true;
		_attempt.victim = a_victim->GetHandle();
		_attempt.candidates = std::move(a_cands);
		_attempt.weaponClass = a_wc;
		_attempt.test = a_test;
		PrepareVictim(a_victim);
		// like Kaputt: play right away, in the same frame as the trigger (key press or hit)
		_attempt.prepped = true;
		_attempt.immediate = true;
		auto player = RE::PlayerCharacter::GetSingleton();
		if (player && !TryNext(player)) {
			Finish(false, "\xE8\x99\x95\xE6\xB1\xBA\xE5\x8B\x95\xE7\x95\xAB\xE5\x95\x9F\xE5\x8B\x95\xE5\xA4\xB1\xE6\x95\x97");
		}
		_attempt.immediate = false;
	}

	bool Executor::TryNext(RE::PlayerCharacter* a_player)
	{
		auto& s = Settings::Get();
		auto  victim = _attempt.victim.get();
		auto  process = a_player->GetActorRuntimeData().currentProcess;
		if (!victim || victim->IsDead() || !process) {
			logger::info("  no living victim");
			return false;
		}
		// The randomly chosen animation gets a few chances: a refusal usually only means the player could not
		// start an idle at that instant (mid-swing, landing...). Falling straight through to the next one in the
		// same frame funnelled every execution into the same couple of animations.
		while (_attempt.index < _attempt.candidates.size() && _attempt.tries < std::max(1, s.maxRetries) + 2) {
			auto f = _attempt.candidates[_attempt.index];
			auto idle = f->idles.front();
			for (auto cand : f->idles) {
				int failFn = 0;
				if (IdleConditionsPass(cand, a_player, victim.get(), failFn)) {
					idle = cand;
					break;
				}
			}
			FaceEachOther(a_player, victim.get(), f->GetCategory());
			bool ok = false;
			{
				ConditionStrip strip(idle);
				ok = process->PlayIdle(a_player, idle, victim.get());
			}
			_attempt.current = f;
			_attempt.timer = 0;
			_attempt.started = false;
			if (ok) {
				logger::info("  try {} ({}) -> sent", f->event, idle->GetFormEditorID());
				// animations that kept refusing while this one was accepted really do not work here
				for (auto r : _attempt.refusedList) {
					r->fails[_attempt.weaponClass]++;
				}
				_attempt.refusedList.clear();
				++_attempt.index;
				++_attempt.tries;
				_attempt.refusals = 0;
				return true;
			}
			if (_attempt.immediate && _attempt.index + 1 < _attempt.candidates.size() && _attempt.tries < 3) {
				logger::info("  try {} ({}) -> refused, next", f->event, idle->GetFormEditorID());
				++_attempt.index;
				++_attempt.tries;
				continue;
			}
			if (_attempt.immediate) {
				_attempt.index = 0;
				_attempt.tries = 0;
			}
			if (++_attempt.refusals < 3) {
				_attempt.prepped = false;
				_attempt.prepTimer = -0.12f;
				return true;
			}
			logger::info("  try {} ({}) -> refused 3 times", f->event, idle->GetFormEditorID());
			_attempt.refusedList.push_back(f);
			_attempt.refusals = 0;
			++_attempt.index;
			++_attempt.tries;
		}
		return false;
	}

	void Executor::UpdateAttempt(RE::PlayerCharacter* a_player, float a_dt)
	{
		if (!_attempt.active) {
			return;
		}
		auto& s = Settings::Get();
		auto  victim = _attempt.victim.get();
		if (!victim) {
			_attempt.active = false;
			return;
		}

		// 0. game killmove requested: did the game start it?
		if (_attempt.engine && !_attempt.started) {
			_attempt.timer += a_dt;
			if (a_player->IsInKillMove() || IsSynced(a_player)) {
				_attempt.engine = false;  // from here on it is an ordinary running pair
				_attempt.timer = 0;
				return;
			}
			if (_attempt.timer < 0.35f) {
				return;
			}
			if (_attempt.candidates.empty()) {
				_attempt.active = false;  // started from an attack: nothing to fall back to
				return;
			}
			// not taken: give the health back and play one directly
			logger::info("  game killmove not started, playing directly");
			if (auto avo = victim->AsActorValueOwner(); avo && _attempt.engineHp > 1.0f) {
				avo->RestoreActorValue(RE::ActorValue::kHealth, _attempt.engineHp - 1.0f);
			}
			auto cands = std::move(_attempt.candidates);
			int  wc = _attempt.weaponClass;
			_attempt.active = false;
			Begin(victim.get(), std::move(cands), wc, false);
			return;
		}

		// 1. wait (briefly) until the victim is not attacking, staggering, recoiling or blocking -
		//    a graph in one of those states ignores the paired animation
		if (!_attempt.prepped) {
			_attempt.prepTimer += a_dt;
			bool busy = VictimBusy(victim.get());
			if (busy && _attempt.prepTimer < 0.7f) {
				if (std::fmod(_attempt.prepTimer, 0.1f) < a_dt) {
					PrepareVictim(victim.get());
				}
				return;
			}
			if (_attempt.prepTimer < 0.08f) {
				return;
			}
			_attempt.prepped = true;
			if (busy) {
				logger::info("  victim still busy after waiting, trying anyway");
			}
			if (!TryNext(a_player)) {
				Finish(false, "\xE8\x99\x95\xE6\xB1\xBA\xE5\x8B\x95\xE7\x95\xAB\xE5\x95\x9F\xE5\x8B\x95\xE5\xA4\xB1\xE6\x95\x97");  // 處決動畫啟動失敗
			}
			return;
		}

		bool victimIn = IsSynced(victim.get());
		bool playerIn = a_player->IsInKillMove() || IsSynced(a_player);

		// 2. after cancelling a one-sided animation, make sure the player is really released
		if (_attempt.oneSided) {
			_attempt.timer += a_dt;
			if (playerIn && _attempt.timer < 1.5f) {
				if (std::fmod(_attempt.timer, 0.25f) < a_dt) {
					ReleasePair(a_player, victim.get(), s.cancelUnpaired);
				}
				return;
			}
			ReleasePair(a_player, victim.get(), false);
			_attempt.oneSided = false;
			_attempt.timer = 0;
			if (!s.cancelUnpaired && playerIn) {
				return;  // (cancel off) still playing alone: keep waiting in this state
			}
			if (_attempt.test || !TryNext(a_player)) {
				Finish(false, "\xE8\x99\x95\xE6\xB1\xBA\xE5\x8B\x95\xE7\x95\xAB\xE5\x95\x9F\xE5\x8B\x95\xE5\xA4\xB1\xE6\x95\x97");  // 處決動畫啟動失敗
			}
			return;
		}

		if (!_attempt.started) {
			_attempt.timer += a_dt;
			if (victimIn) {
				_attempt.started = true;
				if (_attempt.current) {
					_attempt.current->success++;
					_lastResult = _attempt.current->event;
					_recent.push_back(_attempt.current);
					while (_recent.size() > 3) {
						_recent.pop_front();
					}
				}
				return;
			}
			// a working pair syncs the victim within a frame or two; the player playing alone for longer means
			// the victim's graph ignored it - stop it at once so it is barely visible, then try another
			float limit = playerIn ? 0.25f : 0.8f;
			if (_attempt.timer < limit) {
				return;
			}
			if (_attempt.current) {
				_attempt.current->fails[_attempt.weaponClass]++;
				logger::info("  {} did not pair (player in anim {}, player synced {}, victim killmove flag {}, victim synced {})",
					_attempt.current->event, a_player->IsInKillMove(), IsSynced(a_player), victim->IsInKillMove(), victimIn);
			}
			if (playerIn) {
				ReleasePair(a_player, victim.get(), s.cancelUnpaired);
				_attempt.oneSided = true;
				_attempt.timer = 0;
				return;
			}
			// nothing started at all (the idle was accepted but never ran): this animation does not work here
			if (_attempt.current) {
				_attempt.current->fails[_attempt.weaponClass]++;
			}
			// safe to try another one right away
			ReleasePair(a_player, victim.get(), false);
			PrepareVictim(victim.get());
			if (_attempt.test || !TryNext(a_player)) {
				Finish(false, "\xE8\x99\x95\xE6\xB1\xBA\xE5\x8B\x95\xE7\x95\xAB\xE5\x95\x9F\xE5\x8B\x95\xE5\xA4\xB1\xE6\x95\x97");
			}
			return;
		}

		// 3. running: wait until neither is in the pair any more
		if (playerIn || victimIn) {
			_attempt.endTimer = 0;
			return;
		}
		_attempt.endTimer += a_dt;
		if (_attempt.endTimer < 0.3f) {
			return;
		}
		ClearKillMove(a_player);
		if (!victim->IsDead()) {
			ClearKillMove(victim.get());
			if (s.ensureKill && (s.allowEssential || !victim->IsEssential())) {
				// the engine's own kill with an instant ragdoll - damage alone does not run the death check
				logger::info("  victim survived the animation - finishing them");
				victim->KillImpl(a_player, 0.0f, true, true);
			}
		}
		Finish(true, nullptr);
	}

	void Executor::ReleasePair(RE::PlayerCharacter* a_player, RE::Actor* a_victim, bool a_cancelAnim)
	{
		if (a_cancelAnim) {
			// end the paired state the way the animations themselves do, then return to the default state
			static const RE::BSFixedString kPairEnd("PairEnd");
			static const RE::BSFixedString kDefault("IdleForceDefaultState");
			a_player->NotifyAnimationGraph(kPairEnd);
			a_player->NotifyAnimationGraph(kDefault);
		}
		ClearKillMove(a_player);
		if (a_victim) {
			ClearKillMove(a_victim);
		}
	}

	void Executor::OnPlayerHit(RE::Actor* a_victim, RE::HitData& a_hit, bool a_after)
	{
		auto& s = Settings::Get();
		auto  player = RE::PlayerCharacter::GetSingleton();
		if (!s.enabled || !s.hitTrigger || !player || !a_victim) {
			return;
		}
		if (a_after) {
			auto pending = _hitVictim.get();
			_hitVictim.reset();
			if (pending && pending.get() == a_victim && !a_victim->IsDead() && IsExecutable(player, a_victim)) {
				logger::info("Hit trigger on {}", a_victim->GetDisplayFullName());
				StartExecution(player, a_victim);
			}
			return;
		}
		auto aggressor = a_hit.aggressor.get();
		if (!aggressor || aggressor.get() != player || _attempt.active || a_victim->IsDead() ||
			player->IsInKillMove() || IsSynced(player) || a_victim->IsInKillMove() || IsSynced(a_victim)) {
			return;
		}
		auto avo = a_victim->AsActorValueOwner();
		if (!avo) {
			return;
		}
		float hp = avo->GetActorValue(RE::ActorValue::kHealth);
		float maxHp = avo->GetPermanentActorValue(RE::ActorValue::kHealth);
		float mult = DamageMult();
		float after = hp - a_hit.totalDamage * mult;
		if (maxHp <= 0.0f || after / maxHp > s.healthThreshold) {
			return;
		}
		std::uniform_real_distribution<float> roll(0.0f, 100.0f);
		if (roll(_rng) >= s.hitChance) {
			return;
		}
		// like VioLens / Kaputt post-hit: the hit leaves them alive and does not stagger, the finisher does the rest
		if (after < 1.0f && mult > 0.0f) {
			a_hit.totalDamage = std::max(0.0f, (hp - 1.0f) / mult);
		}
		a_hit.stagger = 0.0f;
		_hitVictim = a_victim->GetHandle();
	}

	void Executor::Unstuck()
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return;
		}
		static const RE::BSFixedString kPairEnd("PairEnd");
		static const RE::BSFixedString kDefault("IdleForceDefaultState");
		player->NotifyAnimationGraph(kPairEnd);
		player->NotifyAnimationGraph(kDefault);
		ClearKillMove(player);
		if (auto v = _attempt.victim.get()) {
			ClearKillMove(v.get());
		}
		_attempt = Attempt{};
		logger::info("Unstuck: player animation state reset");
	}

	void Executor::Finish(bool a_ok, const char* a_msg)
	{
		_attempt.active = false;
		if (!a_ok) {
			_lastResult = "failed";
		}
		Catalog::Get().SaveUser();
		if (a_msg) {
			Notify(a_msg);
		}
	}

	void Executor::Tick(RE::PlayerCharacter* a_player, float a_dt)
	{
		auto& s = Settings::Get();
		_clock += a_dt;
		if (!s.enabled) {
			_marked.clear();
			return;
		}
		_scanTimer -= a_dt;
		if (_scanTimer <= 0.0f) {
			_scanTimer = 0.2f;
			UpdateMarks(a_player);
		}
		UpdateBleedout();
		UpdateStagger(a_player);
		if (_executeRequested.exchange(false)) {
			StartExecution(a_player);
		}
		if (auto key = PendingTest(); !key.empty()) {
			RequestTest("");
			StartTest(a_player, key);
		}
		UpdateAttempt(a_player, a_dt);
		UpdateArm(a_player);

		// watchdog: a kill-move flag left on the player with no pair running blocks every action
		if (!_attempt.active && a_player->IsInKillMove() && !IsSynced(a_player)) {
			_stuckTimer += a_dt;
			if (_stuckTimer > 3.0f) {
				logger::info("Player kill-move flag stuck without a pair - clearing it");
				ClearKillMove(a_player);
				_stuckTimer = 0;
			}
		} else {
			_stuckTimer = 0;
		}
	}
}
