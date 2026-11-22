#include "PCH.h"
#include "Core.h"

#include "Settings.h"

#include <MinHook.h>

#ifdef GetObject
#	undef GetObject  // windows.h (UNICODE) renames it to GetObjectW
#endif

namespace Zan
{
	const char* SourceName(int a_source)
	{
		static const char* n[] = { "Vanilla", "Low-blow", "Thicc Thighs" };
		return a_source >= 0 && a_source < kSourceCount ? n[a_source] : "?";
	}

	namespace
	{
		std::string Lower(std::string a_s)
		{
			std::transform(a_s.begin(), a_s.end(), a_s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_s;
		}

		std::string EditorID(RE::TESForm* a_form)
		{
			const char* id = a_form ? a_form->GetFormEditorID() : nullptr;
			return id ? id : "";
		}

		void Notify(const char* a_msg)
		{
			using func_t = void (*)(const char*, const char*, bool);
			static REL::Relocation<func_t> func{ RELOCATION_ID(52050, 52933) };
			func(a_msg, nullptr, true);
		}

		RE::TESForm* PlayerBase()
		{
			static RE::TESForm* base = RE::TESForm::LookupByID(0x7);
			return base;
		}

		// conditions that only gate *random* selection (chance, the killmove mods' randomizer globals) and gates
		// keyed on the player (ours) - everything else (weapon, angle, race, sneaking...) is kept
		bool IsRandomGate(const RE::TESConditionItem* a_item)
		{
			using F = RE::FUNCTION_DATA::FunctionID;
			auto fn = a_item->data.functionData.function.get();
			if (fn == F::kGetRandomPercent || fn == F::kGetGlobalValue) {
				return true;
			}
			return fn == F::kGetIsID && a_item->data.functionData.params[0] == PlayerBase();
		}

		// conditions about the attack itself (attack state, power attack, "this blow kills"): they only become true
		// once the attack Zan sends is running with the victim at 1 HP, so they are not checked in advance
		bool IsAttackStateCheck(const RE::TESConditionItem* a_item)
		{
			using F = RE::FUNCTION_DATA::FunctionID;
			switch (a_item->data.functionData.function.get()) {
			case F::kShouldAttackKill:
			case F::kGetAttackState:
			case F::kIsAttacking:
			case F::kIsPowerAttacking:
			case F::kGetGraphVariableInt:
				return true;
			default:
				return false;
			}
		}

		// Evaluates an idle and its parents (player = subject, victim = target), skipping the random gates.
		// OR items bind to their neighbours, AND separates groups - the same rule the game uses.
		bool ConditionsPass(RE::TESIdleForm* a_idle, RE::Actor* a_subject, RE::Actor* a_target, int* a_failFn = nullptr)
		{
			int firstFail = -1;
			int guard = 0;
			for (auto i = a_idle; i && guard < 32; i = i->parentIdle, ++guard) {
				bool groupOk = false;
				bool groupHas = false;
				for (auto item = i->conditions.head; item; item = item->next) {
					bool ok = true;
					if (!IsRandomGate(item) && !IsAttackStateCheck(item)) {
						RE::ConditionCheckParams params(a_subject, a_target);
						ok = item->IsTrue(params);
					}
					if (!ok && firstFail < 0) {
						firstFail = static_cast<int>(item->data.functionData.function.get());
					}
					groupOk = groupOk || ok;
					groupHas = true;
					if (!item->data.flags.isOR) {
						if (!groupOk) {
							if (a_failFn) {
								*a_failFn = firstFail;
							}
							return false;
						}
						groupOk = false;
						groupHas = false;
						firstFail = -1;
					}
				}
				if (groupHas && !groupOk) {
					if (a_failFn) {
						*a_failFn = firstFail;
					}
					return false;
				}
				firstFail = -1;
			}
			return true;
		}

		// NiFormArray members are private in CommonLib; same layout as NiTLargeArray
		struct RawFormArray
		{
			void*         vtbl;
			RE::TESForm** data;
			std::uint32_t capacity;
			std::uint32_t freeIdx;
			std::uint32_t size;
			std::uint32_t growBy;
		};
		static_assert(sizeof(RawFormArray) == sizeof(RE::NiFormArray));

		bool InPair(RE::Actor* a_actor)
		{
			if (!a_actor) {
				return false;
			}
			if (a_actor->IsInKillMove()) {
				return true;
			}
			static const RE::BSFixedString kSynced("bIsSynced");
			bool v = false;
			return a_actor->GetGraphVariableBool(kSynced, v) && v;
		}

		bool IsCreatureText(const std::string& a_text)
		{
			static const char* words[] = { "bear", "wolf", "spider", "troll", "sabre", "sabrecat", "mammoth", "giant", "chaurus",
				"dragon", "werewolf", "vampirelord", "deathhound", "gargoyle", "horker", "riekling", "lurker", "netch", "boar",
				"husky", "dog", "horse", "skeever", "mudcrab", "atronach", "falmer", "draugr", "scrib", "ashhopper", "centurion",
				"spriggan", "hagraven", "creature" };
			for (auto w : words) {
				if (a_text.find(w) != std::string::npos) {
					return true;
				}
			}
			return false;
		}

		using CamUpdateFn = void (*)(RE::PlayerCamera*);
		CamUpdateFn g_camUpdate = nullptr;

		// camera update: marker positions need this frame's camera
		void HookedCamUpdate(RE::PlayerCamera* a_this)
		{
			g_camUpdate(a_this);
			auto ui = RE::UI::GetSingleton();
			if (ui && ui->GameIsPaused()) {
				return;
			}
			Core::Get().UpdateMarkerScreen();
		}

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
				Core::Get().Tick(player, std::clamp(*deltaTime, 0.0f, 0.1f));
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Core::InstallHooks()
	{
		REL::Relocation<std::uintptr_t> mainUpdate{ RELOCATION_ID(35551, 36544), REL::VariantOffset(0x11F, 0x160, 0x0) };
		SKSE::AllocTrampoline(14);
		MainUpdateHook::func = SKSE::GetTrampoline().write_call<5>(mainUpdate.address(), MainUpdateHook::thunk);
		logger::info("Main update hook installed");

		REL::Relocation<std::uintptr_t> cam{ RELOCATION_ID(49852, 50784) };
		MH_STATUS st = MH_Initialize();
		if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
			logger::error("MinHook init failed: no marker");
			return;
		}
		auto addr = reinterpret_cast<LPVOID>(cam.address());
		if (MH_CreateHook(addr, reinterpret_cast<LPVOID>(&HookedCamUpdate), reinterpret_cast<LPVOID*>(&g_camUpdate)) != MH_OK ||
			MH_EnableHook(addr) != MH_OK) {
			logger::error("Camera hook failed: no marker");
		}
	}

	// ---------------------------------------------------------------- finding the killmoves

	void Core::Build()
	{
		std::vector<RE::TESIdleForm*> idles;
		{
			auto [map, lock] = RE::TESForm::GetAllForms();
			RE::BSReadLockGuard guard{ lock.get() };
			if (map) {
				for (auto& [id, form] : *map) {
					if (form && form->GetFormType() == RE::FormType::Idle) {
						idles.push_back(static_cast<RE::TESIdleForm*>(form));
					}
				}
			}
		}

		auto chainText = [](RE::TESIdleForm* a_idle) {
			std::string t;
			int         guard = 0;
			for (auto i = a_idle; i && guard < 32; i = i->parentIdle, ++guard) {
				t += Lower(EditorID(i)) + " ";
			}
			return t;
		};

		std::vector<RE::TESIdleForm*> others;
		for (auto idle : idles) {
			if (!idle->parentIdle) {
				continue;  // not in the idle tree: the game's killmove system can never pick it
			}
			std::string ev = idle->animEventName.c_str() ? idle->animEventName.c_str() : "";
			std::string evl = Lower(ev);
			std::string chain = chainText(idle);
			bool        killmove = !ev.empty() && evl.starts_with("pa_") &&
			                (chain.find("killmove") != std::string::npos || evl.find("killmove") != std::string::npos);
			if (!killmove) {
				// a real idle outside every killmove branch (an attack, a bash...): its parents are attack-tree nodes.
				// Event-less grouping nodes inside the killmove branches don't count.
				if (!ev.empty() && chain.find("killmove") == std::string::npos) {
					others.push_back(idle);
				}
				continue;
			}
			if (IsCreatureText(evl + " " + chain)) {
				continue;
			}
			Killmove km;
			km.idle = idle;
			km.event = ev;
			km.editorID = EditorID(idle);
			auto file = idle->GetFile(0);
			km.file = file ? std::string(file->GetFilename()) : "";
			auto fl = Lower(km.file);
			if (fl.find("lbk") != std::string::npos || fl.find("low") != std::string::npos) {
				km.source = kLowBlow;
			} else if (fl.find("snusnu") != std::string::npos || fl.find("tttl") != std::string::npos || fl.find("thicc") != std::string::npos) {
				km.source = kThicc;
			}
			_all.push_back(std::move(km));
		}

		// tree nodes that also lead to ordinary idles (the attack trees of BFCO, SCSI...): never touched
		for (auto idle : others) {
			int guard = 0;
			for (auto n = idle->parentIdle; n && guard < 32; n = n->parentIdle, ++guard) {
				_attackNodes.insert(n);
			}
		}

		// which attack leads to each one: from the attack-tree nodes above it
		int skipped = 0;
		for (auto it = _all.begin(); it != _all.end();) {
			std::string up;
			int         guard = 0;
			for (auto n = it->idle->parentIdle; n && guard < 32; n = n->parentIdle, ++guard) {
				if (_attackNodes.contains(n) || !n->parentIdle) {
					up += Lower(EditorID(n)) + " ";
				}
			}
			if (up.find("bash") != std::string::npos || up.find("sprint") != std::string::npos || up.find("mount") != std::string::npos && up.find("nonmount") == std::string::npos) {
				it = _all.erase(it);
				++skipped;
				continue;
			}
			bool left = up.find("left") != std::string::npos;
			bool power = up.find("power") != std::string::npos;
			bool dual = up.find("dual") != std::string::npos;
			it->attack = dual ? kDualPower : left ? (power ? kLeftPower : kLeft) : (power ? kRightPower : kRight);
			++it;
		}
		if (skipped) {
			logger::info("{} bash / sprint / mounted killmoves left out", skipped);
		}

		// each killmove's branch top, and the attack-tree nodes holding branches (normal, power, left... attacks)
		std::unordered_set<RE::TESIdleForm*> hosts;
		for (auto& km : _all) {
			int guard = 0;
			auto n = km.idle;
			while (n->parentIdle && !_attackNodes.contains(n->parentIdle) && guard++ < 32) {
				n = n->parentIdle;
			}
			km.top = n;
			if (n->parentIdle && n->parentIdle->childIdles) {
				hosts.insert(n->parentIdle);
			}
		}
		_hosts.assign(hosts.begin(), hosts.end());
		for (auto h : _hosts) {
			logger::info("  killmove host: {} ({:08X})", EditorID(h), h->GetFormID());
		}

		// the gate: GetIsID(player) != 1 - closed for the player, open for everyone else.
		// Its value is set to 2 (always true) while that killmove is the one picked.
		for (auto& km : _all) {
			auto item = new RE::TESConditionItem();
			item->data.functionData.function = RE::FUNCTION_DATA::FunctionID::kGetIsID;
			item->data.functionData.params[0] = PlayerBase();
			item->data.functionData.params[1] = nullptr;
			item->data.object = RE::CONDITIONITEMOBJECT::kSelf;
			item->data.flags.opCode = RE::CONDITION_ITEM_DATA::OpCode::kNotEqualTo;
			item->data.flags.isOR = false;
			item->data.comparisonValue.f = 1.0f;
			item->next = km.idle->conditions.head;
			km.idle->conditions.head = item;
			km.gate = item;
		}

		int kinds[kAttackKindCount]{};
		for (auto& km : _all) {
			kinds[km.attack]++;
		}
		logger::info("By attack: {} normal, {} power, {} left, {} left power, {} dual power", kinds[kRight], kinds[kRightPower], kinds[kLeft],
			kinds[kLeftPower], kinds[kDualPower]);
		{
			std::string list = Settings::Get().blocked, item;
			std::stringstream ss(list);
			while (std::getline(ss, item, ',')) {
				if (!item.empty()) {
					_blocked.insert(item);
				}
			}
			if (!_blocked.empty()) {
				logger::info("{} animation(s) blocked by you", _blocked.size());
			}
		}
		auto c = CountBySource();
		logger::info("Found {} killmoves in the idle tree: {} vanilla / VioLens, {} Low-blow, {} Thicc Thighs ({} attack-tree nodes left alone)",
			_all.size(), c[kVanilla], c[kLowBlow], c[kThicc], _attackNodes.size());
		if (c[kLowBlow] == 0) {
			logger::info("  no Low-blow killmoves in the tree - VL-LBKTEST.esp (with VioLens SE.esp) puts them there");
		}
		if (c[kThicc] == 0) {
			logger::info("  no Thicc Thighs killmoves in the tree - its VioLens version (SnuSnu-VioLensed.esp) puts them there");
		}
	}

	void Core::Block(Killmove* a_km)
	{
		if (!a_km || !_blocked.insert(a_km->editorID).second) {
			return;
		}
		auto& s = Settings::Get();
		s.blocked += (s.blocked.empty() ? "" : ",") + a_km->editorID;
		s.Save();
		logger::info("Blocked {} ({})", a_km->editorID, a_km->event);
	}

	void Core::ClearBlocked()
	{
		_blocked.clear();
		Settings::Get().blocked.clear();
		Settings::Get().Save();
	}

	std::array<int, kSourceCount> Core::CountBySource() const
	{
		std::array<int, kSourceCount> c{};
		for (auto& km : _all) {
			c[km.source]++;
		}
		return c;
	}

	// ---------------------------------------------------------------- marks

	bool Core::IsExecutable(RE::PlayerCharacter* a_player, RE::Actor* a_actor) const
	{
		auto& s = Settings::Get();
		if (!a_actor || a_actor == a_player || a_actor->IsDead() || !a_actor->Is3DLoaded() || a_actor->IsEssential() ||
			!a_actor->IsHostileToActor(a_player) || InPair(a_actor)) {
			return false;
		}
		if (s.humanoidOnly && !a_actor->HasKeywordString("ActorTypeNPC")) {
			return false;
		}
		auto avo = a_actor->AsActorValueOwner();
		if (!avo) {
			return false;
		}
		float cur = avo->GetActorValue(RE::ActorValue::kHealth);
		float max = avo->GetPermanentActorValue(RE::ActorValue::kHealth) +
		            a_actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kHealth);
		if (max <= 0.0f) {
			return false;
		}
		return cur / max <= s.healthThreshold && a_actor->GetPosition().GetDistance(a_player->GetPosition()) < 3000.0f;
	}

	void Core::UpdateMarks(RE::PlayerCharacter* a_player)
	{
		std::unordered_map<RE::FormID, Mark> now;
		if (auto pl = RE::ProcessLists::GetSingleton()) {
			pl->ForEachHighActor([&](RE::Actor* a) {
				if (IsExecutable(a_player, a)) {
					now[a->GetFormID()] = Mark{ a->GetHandle() };
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}
		_marked = std::move(now);
	}

	RE::Actor* Core::PickVictim(RE::PlayerCharacter* a_player)
	{
		auto&      s = Settings::Get();
		RE::Actor* best = nullptr;
		float      bestD = s.range;
		for (auto& [id, m] : _marked) {
			auto a = m.handle.get();
			if (!a || a->IsDead()) {
				continue;
			}
			float d = a->GetPosition().GetDistance(a_player->GetPosition());
			float ang = std::abs(a_player->GetHeadingAngle(a->GetPosition(), true));
			float dz = std::abs(a->GetPositionZ() - a_player->GetPositionZ());
			if (d <= bestD && ang <= s.cone && dz <= 50.0f) {
				bestD = d;
				best = a.get();
			}
		}
		return best;
	}

	void Core::UpdateMarkerScreen()
	{
		std::vector<MarkerDraw> out;
		auto&                   s = Settings::Get();
		auto                    cam = RE::Main::WorldRootCamera();
		auto                    player = RE::PlayerCharacter::GetSingleton();
		if (s.enabled && s.markerEnabled && cam && player) {
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
				RE::NiPoint3 fwd = player->GetPosition() - cam->world.translate;
				RE::NiPoint3 to = p - cam->world.translate;
				fwd.z = to.z = 0.0f;
				bool front = fwd.Length() < 1.0f || fwd.Dot(to) > 0.0f;
				if (front && x > -0.1f && x < 1.1f && y > -0.1f && y < 1.1f) {
					out.push_back({ x, 1.0f - y });
				}
			}
		}
		std::scoped_lock l(_markerLock);
		_markers = std::move(out);
	}

	// ---------------------------------------------------------------- choosing

	Killmove* Core::Choose(RE::PlayerCharacter* a_player, RE::Actor* a_victim, std::initializer_list<int> a_kinds, bool a_log)
	{
		auto&                                         s = Settings::Get();
		std::array<std::vector<Killmove*>, kSourceCount> pool;
		int                                           checked = 0;
		std::map<int, int>                            failCount;  // condition function -> killmoves it stopped
		// left-hand attacks only with a weapon in the left hand, or with both hands empty (fists)
		auto isWeapon = [](RE::TESForm* f) { return f && f->IsWeapon(); };
		auto rightObj = a_player->GetEquippedObject(false);
		auto leftObj = a_player->GetEquippedObject(true);
		bool leftOk = isWeapon(leftObj) || (!leftObj && !rightObj);
		bool dualOk = isWeapon(leftObj) && isWeapon(rightObj);
		for (auto& km : _all) {
			if (std::find(a_kinds.begin(), a_kinds.end(), km.attack) == a_kinds.end()) {
				continue;
			}
			// a left-hand branch needs a weapon (or fists) in the left hand - its own conditions look at it
			if ((km.attack == kLeft || km.attack == kLeftPower) && !leftOk || km.attack == kDualPower && !dualOk) {
				continue;
			}
			if (_blocked.contains(km.editorID)) {
				continue;
			}
			// one that the game kept refusing this session is left out (unless it worked before)
			if (km.missed >= 5 && km.started == 0) {
				continue;
			}
			++checked;
			int failFn = -1;
			if (ConditionsPass(km.idle, a_player, a_victim, &failFn)) {
				pool[km.source].push_back(&km);
			} else {
				failCount[failFn]++;
			}
		}
		const int weights[kSourceCount] = { s.weightVanilla, s.weightLowBlow, s.weightThicc };
		std::vector<int> sources;
		std::vector<int> w;
		for (int i = 0; i < kSourceCount; ++i) {
			if (!pool[i].empty() && weights[i] > 0) {
				sources.push_back(i);
				w.push_back(weights[i]);
			}
		}
		static const char* kKindNames[kAttackKindCount] = { "light", "heavy", "left", "left heavy", "dual" };
		logger::info("  {} attack: {} vanilla, {} Low-blow, {} Thicc usable (of {} checked)", kKindNames[*a_kinds.begin()],
			pool[kVanilla].size(), pool[kLowBlow].size(), pool[kThicc].size(), checked);
		if (sources.empty()) {
			if (!a_log) {
				return nullptr;
			}
			std::vector<std::pair<int, int>> top(failCount.begin(), failCount.end());
			std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
			std::string t;
			for (std::size_t i = 0; i < top.size() && i < 6; ++i) {
				t += std::format(" fn{}x{}", top[i].first, top[i].second);
			}
			logger::info("  stopped by (condition function x killmoves):{}", t);
			return nullptr;
		}
		// 1. the source, by weight - so Low-blow, Thicc and vanilla come up equally often however many each has
		std::discrete_distribution<int> pickSource(w.begin(), w.end());
		auto&                           list = pool[sources[pickSource(_rng)]];
		// 2. an animation from it, skipping the last few played while there is anything else
		std::vector<Killmove*> fresh;
		for (auto km : list) {
			if (std::find(_recent.begin(), _recent.end(), km->event) == _recent.end()) {
				fresh.push_back(km);
			}
		}
		auto& from = fresh.empty() ? list : fresh;
		std::uniform_int_distribution<std::size_t> pick(0, from.size() - 1);
		return from[pick(_rng)];
	}

	// Lifts the random gates (chance, randomizer globals, our player gate) on the picked killmove's own branch,
	// stopping where the branch joins an attack tree. Everything is put back by Open(nullptr, false).
	void Core::Open(Killmove* a_km, bool a_open)
	{
		if (!a_open) {
			for (auto it = _savedArrays.rbegin(); it != _savedArrays.rend(); ++it) {
				auto raw = static_cast<RawFormArray*>(it->array);
				raw->data = it->data;  // the temporary buffer is not freed: something may still be iterating it
				raw->capacity = it->capacity;
				raw->freeIdx = it->freeIdx;
				raw->size = it->size;
			}
			_savedArrays.clear();
			for (auto it = _saved.rbegin(); it != _saved.rend(); ++it) {
				std::memcpy(&it->first->data, it->second.data(), it->second.size());
			}
			_saved.clear();
			for (auto& km : _all) {
				km.gate->data.comparisonValue.f = 1.0f;
			}
			return;
		}
		a_km->gate->data.comparisonValue.f = 2.0f;
		// The random gates (chance, VioLens' mode / randomizer globals, "not the player") on the picked killmove's
		// branch are turned into an always-true check - not removed: many sit in OR groups, and removing an OR
		// partner can make the rest of its group fail (that is what stopped v1.1). Stops at the attack tree.
		int guard = 0;
		for (auto n = a_km->idle; n && guard < 32 && !_attackNodes.contains(n); n = n->parentIdle, ++guard) {
			for (auto item = n->conditions.head; item; item = item->next) {
				if (item == a_km->gate || !IsRandomGate(item)) {
					continue;
				}
				std::array<std::byte, sizeof(RE::CONDITION_ITEM_DATA)> copy;
				std::memcpy(copy.data(), &item->data, copy.size());
				_saved.emplace_back(item, copy);
				bool isOR = item->data.flags.isOR;
				// GetIsID(player) != 2 - always true (GetIsID is 0 or 1)
				item->data.functionData.function = RE::FUNCTION_DATA::FunctionID::kGetIsID;
				item->data.functionData.params[0] = PlayerBase();
				item->data.functionData.params[1] = nullptr;
				item->data.object = RE::CONDITIONITEMOBJECT::kSelf;
				item->data.flags.global = false;
				item->data.flags.usesAliases = false;
				item->data.flags.usePackData = false;
				item->data.flags.swapTarget = false;
				item->data.flags.opCode = RE::CONDITION_ITEM_DATA::OpCode::kNotEqualTo;
				item->data.flags.isOR = isOR;
				item->data.comparisonValue.f = 2.0f;
			}
		}

	}

	void Core::Dump(Killmove* a_km, RE::PlayerCharacter* a_player, RE::Actor* a_victim)
	{
		if (_dumps++ >= 6) {
			return;
		}
		logger::info("  path of {} ({:08X}, {}):", a_km->editorID, a_km->idle->GetFormID(), a_km->file);
		int guard = 0;
		for (auto n = a_km->idle; n && guard < 32; n = n->parentIdle, ++guard) {
			std::string conds;
			for (auto item = n->conditions.head; item; item = item->next) {
				RE::ConditionCheckParams params(a_player, a_victim);
				bool ok = item->IsTrue(params);
				conds += std::format(" [fn{} op{} {:.2f}{}{}]", static_cast<int>(item->data.functionData.function.get()),
					static_cast<int>(item->data.flags.opCode), item->data.comparisonValue.f, item->data.flags.isOR ? " OR" : "",
					ok ? " ok" : " FAIL");
			}
			logger::info("    {} ({:08X}){}:{}", EditorID(n), n->GetFormID(), _attackNodes.contains(n) ? " [attack tree]" : "",
				conds.empty() ? " (none)" : conds);
		}
	}

	// ---------------------------------------------------------------- the key

	void Core::OnKey()
	{
		auto& s = Settings::Get();
		auto  player = RE::PlayerCharacter::GetSingleton();
		if (!s.enabled || !player || _pending.active || InPair(player)) {
			return;
		}
		auto victim = PickVictim(player);
		if (!victim) {
			Notify("\xE9\x99\x84\xE8\xBF\x91\xE6\xB2\x92\xE6\x9C\x89\xE5\x8F\xAF\xE8\x99\x95\xE6\xB1\xBA\xE7\x9A\x84\xE6\x95\xB5\xE4\xBA\xBA");  // 附近沒有可處決的敵人
			return;
		}
		logger::info("Execute {}:", victim->GetDisplayFullName());
		_swingStarted = false;
		// the game only turns a blow into a killmove when it kills: the victim goes to 1 HP first
		// (also before choosing - some killmove branches check "this blow kills")
		float hp = 0.0f;
		if (auto avo = victim->AsActorValueOwner()) {
			hp = avo->GetActorValue(RE::ActorValue::kHealth);
			if (hp > 1.0f) {
				avo->DamageActorValue(RE::ActorValue::kHealth, hp - 1.0f);
			}
		}
		// one killmove per attack kind, all opened together: Zan's own swing (light) or the player's own light or
		// heavy attack - whichever comes - finds one ready in its own branch of the tree
		std::vector<Killmove*> picks;
		auto light = Choose(player, victim, { kRight }, true);
		if (light) {
			picks.push_back(light);
		}
		if (auto heavy = Choose(player, victim, { kRightPower }, false)) {
			picks.push_back(heavy);
		}
		if (auto left = Choose(player, victim, { kLeft, kLeftPower }, false)) {
			picks.push_back(left);
		}
		auto km = light ? light : (picks.empty() ? nullptr : picks.front());
		if (!km) {
			if (hp > 1.0f) {
				if (auto avo = victim->AsActorValueOwner()) {
					avo->RestoreActorValue(RE::ActorValue::kHealth, hp - 1.0f);
				}
			}
			Notify("\xE6\xB2\x92\xE6\x9C\x89\xE5\x8F\xAF\xE7\x94\xA8\xE7\x9A\x84\xE8\x99\x95\xE6\xB1\xBA\xE5\x8B\x95\xE4\xBD\x9C");  // 沒有可用的處決動作
			return;
		}
		for (auto p : picks) {
			logger::info("  picked {} - {} ({})", SourceName(p->source), p->event, p->editorID);
			Open(p, true);
		}

		// face the victim: the killmoves' angle checks look at it
		auto pp = player->GetPosition();
		auto vp = victim->GetPosition();
		player->SetHeading(std::atan2(vp.x - pp.x, vp.y - pp.y));

		_pending = Pending{};
		_pending.active = true;
		_pending.victim = victim->GetHandle();
		_pending.pick = km;
		_pending.picks = picks;
		_pending.hp = hp;
		// Zan swings once (and tries twice more if the player is busy). Whether it takes or not, the killmove stays
		// ready for the whole window, so the player's own next attack - light or heavy - can start it too.
		// (v1.3/1.4 cut the running animation to force the swing: that jammed BFCO's attacks, so it is gone.)
		if (s.autoAttack) {
			_pending.sent = SendAttack(player, victim, kRight);
			_pending.refusedIdle = !_pending.sent;
			_pending.tries = _pending.sent ? 0 : 1;
		}
	}

	void Core::SwapLight(RE::PlayerCharacter* a_player, RE::Actor* a_victim)
	{
		auto old = _pending.pick;
		if (!old) {
			return;
		}
		old->missed++;
		_recent.push_back(old->event);  // steer the draw away from it
		auto fresh = Choose(a_player, a_victim, { kRight }, false);
		if (!fresh || fresh == old) {
			return;
		}
		Open(nullptr, false);
		for (auto& p : _pending.picks) {
			if (p == old) {
				p = fresh;
			}
		}
		_pending.pick = fresh;
		for (auto p : _pending.picks) {
			Open(p, true);
		}
		logger::info("  refused - trying {} - {} instead", SourceName(fresh->source), fresh->event);
	}

	bool Core::SendAttack(RE::PlayerCharacter* a_player, RE::Actor* a_victim, int a_kind)
	{
		static const RE::DEFAULT_OBJECT kActions[kAttackKindCount] = { RE::DEFAULT_OBJECT::kActionRightAttack,
			RE::DEFAULT_OBJECT::kActionRightPowerAttack, RE::DEFAULT_OBJECT::kActionLeftAttack, RE::DEFAULT_OBJECT::kActionLeftPowerAttack,
			RE::DEFAULT_OBJECT::kActionDualPowerAttack };
		static const char* kNames[kAttackKindCount] = { "attack", "power attack", "left attack", "left power attack", "dual power attack" };
		auto dom = RE::BGSDefaultObjectManager::GetSingleton();
		auto attack = dom ? dom->GetObject<RE::BGSAction>(kActions[a_kind]) : nullptr;
		auto action = attack ? RE::TESActionData::Create() : nullptr;
		if (!action) {
			return false;
		}
		action->source = RE::NiPointer<RE::TESObjectREFR>(a_player);
		action->target = RE::NiPointer<RE::TESObjectREFR>(a_victim);
		action->action = attack;
		bool ok = action->Process();  // not freed: the game may still use it while the attack runs
		_swingStarted = ok && a_player->IsInKillMove();
		if (!ok) {
			auto ps = a_player->AsActorState();
			auto vs = a_victim->AsActorState();
			logger::info("    (you: attack state {}, staggered {} | enemy: attack state {}, staggered {}, knock {}, distance {:.0f})",
				ps ? static_cast<int>(ps->GetAttackState()) : -1, ps && ps->IsStaggered(), vs ? static_cast<int>(vs->GetAttackState()) : -1,
				vs && vs->IsStaggered(), vs ? static_cast<int>(vs->GetKnockState()) : -1,
				a_player->GetPosition().GetDistance(a_victim->GetPosition()));
		}
		logger::info("  {} {}{}", kNames[a_kind], ok ? "started" : "not accepted yet", a_player->IsInKillMove() ? " - killmove started" : "");
		return ok;
	}

	void Core::Finish(bool a_started)
	{
		auto victim = _pending.victim.get();
		auto km = _pending.pick;
		if (!a_started && km) {
			Dump(km, RE::PlayerCharacter::GetSingleton(), victim.get());  // still open, victim still at 1 HP
		}
		Open(nullptr, false);
		if (!a_started) {
			if (victim && !victim->IsDead() && _pending.hp > 1.0f) {
				if (auto avo = victim->AsActorValueOwner()) {
					avo->RestoreActorValue(RE::ActorValue::kHealth, _pending.hp - 1.0f);
				}
			}
			if (km) {
				km->missed++;
				logger::info("  the game did not start {} ({} misses, attack {})", km->event, km->missed,
					_pending.sent ? "was sent" : "never accepted");
			}
			_lastResult = km ? "did not start: " + km->event : "did not start";
		}
		_pending = Pending{};
	}

	void Core::Tick(RE::PlayerCharacter* a_player, float a_dt)
	{
		_clock += a_dt;
		_scan += a_dt;
		if (_scan >= 0.2f) {
			_scan = 0.0f;
			if (Settings::Get().enabled) {
				UpdateMarks(a_player);
			} else {
				_marked.clear();
			}
		}
		if (!_pending.active) {
			return;
		}
		auto victim = _pending.victim.get();
		if (!_pending.started) {
			_pending.timer += a_dt;
			if (a_player->IsInKillMove()) {
				// the game took it: put every condition back at once
				_pending.started = true;
				Open(nullptr, false);
				// which of the open ones played: Zan's swing reaches the light pick; the player's own attack - unknown
				Killmove* km = _pending.pick;
				_lastCandidates.clear();
				if (_swingStarted && km) {
					_lastCandidates.push_back(km);
				} else {
					_lastCandidates = _pending.picks;
				}
				std::string all;
				for (auto p : _pending.picks) {
					all += (all.empty() ? "" : " / ") + p->event;
					_recent.push_back(p->event);  // all of them count as just played: the next key draws fresh ones
				}
				while (static_cast<int>(_recent.size()) > Settings::Get().noRepeat + 2) {
					_recent.pop_front();
				}
				if (km) {
					km->started++;
				}
				std::string played;
				for (auto c : _lastCandidates) {
					played += (played.empty() ? "" : " or ") + c->event + " [" + c->editorID + "]";
				}
				_lastResult = "started: " + played;
				logger::info("  killmove started: {}", played);
				return;
			}
			// the player's own attack: the attack state goes from "between attacks" to attacking after the key
			if (auto state = a_player->AsActorState()) {
				bool attacking = state->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone;
				if (!attacking) {
					_pending.idleSeen = true;
				} else if (_pending.idleSeen && !_pending.sent) {
					_pending.sent = true;
					logger::info("  your own attack started");
				}
			}
			if (_pending.sent && _pending.sentAt < 0.0f) {
				_pending.sentAt = _pending.timer;
			}
			// the game decides at the start of the attack: no killmove a moment after the attack started means
			// none is coming - give the health back before the blow lands so it is not just a lethal normal hit
			if (_pending.sent && _pending.timer - _pending.sentAt > 0.2f) {
				Finish(false);
				return;
			}
			if (Settings::Get().autoAttack && !_pending.sent && _pending.tries > 0 && _pending.tries < 4 && victim) {
				_pending.retry += a_dt;
				if (_pending.retry >= 0.12f) {
					_pending.retry = 0.0f;
					// a refusal is about this animation (room, angle...) - the next try uses a different one.
					// (v1.8 also stopped an attacking enemy first: that left enemies passive, so it is gone)
					SwapLight(a_player, victim.get());
					_pending.sent = SendAttack(a_player, victim.get(), kRight);
					_pending.tries = _pending.sent ? 0 : _pending.tries + 1;
				}
			}
			if (_pending.timer > Settings::Get().window || !victim || victim->IsDead()) {
				Finish(false);
			}
			return;
		}
		// running: the player in the pair but the enemy not (it kept fighting) = a T-pose. Break it off.
		if (InPair(a_player) && victim && !InPair(victim.get()) && !victim->IsDead()) {
			_pending.lonely += a_dt;
			if (_pending.lonely > 0.4f) {
				static const RE::BSFixedString kDefault("IdleForceDefaultState");
				a_player->NotifyAnimationGraph(kDefault);
				logger::info("  the enemy did not join the pair - stopped it (no T-pose)");
				victim->KillImpl(a_player, 0.0f, true, true);
				_pending = Pending{};
			}
			return;
		}
		_pending.lonely = 0.0f;
		// once it is over, make sure the victim is dead
		if (InPair(a_player) || InPair(victim.get())) {
			_pending.endTimer = 0.0f;
			return;
		}
		_pending.endTimer += a_dt;
		if (_pending.endTimer < 0.4f) {
			return;
		}
		if (victim && !victim->IsDead()) {
			victim->KillImpl(a_player, 0.0f, true, true);
		}
		_pending = Pending{};
	}

	void Core::OnPreLoad()
	{
		if (_pending.active) {
			Finish(_pending.started);
		}
		_marked.clear();
	}
}
