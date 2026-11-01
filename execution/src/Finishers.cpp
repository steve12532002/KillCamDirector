#include "PCH.h"
#include "Finishers.h"

#include "Settings.h"

#include <nlohmann/json.hpp>

namespace ED
{
	namespace
	{
		constexpr auto kKaputtDir = "Data/SKSE/Plugins/Kaputt/anims";
		constexpr auto kMetaFile = "Data/SKSE/Plugins/ExecutionDirector/Finishers.json";
		constexpr auto kUserFile = "Data/SKSE/Plugins/ExecutionDirector/UserFinishers.json";

		std::string Lower(std::string s)
		{
			std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s;
		}

		bool Contains(const std::string& a_hay, std::string_view a_needle) { return a_hay.find(a_needle) != std::string::npos; }

		const char* kCreatureWords[] = { "dragon", "werewolf", "vampirelord", "vamplord", "bear", "troll", "giant", "spider",
			"sabre", "sabercat", "wolf", "chaurus", "horker", "spriggan", "dwarven", "centurion", "sphere", "mammoth",
			"horse", "netch", "lurker", "seeker", "gargoyle", "hagraven", "skeever", "mudcrab", "deathhound", "riekling",
			"ashhopper", "creature", "frostbite", "atronach", "wisp", "boar", "husky", "dog", "goat", "elk", "deer",
			"fox", "cow", "slaughterfish", "ashspawn", "lurker", "karstaag", "benthic", "dragonpriest", "scrib" };

		bool LooksLikeCreature(const std::string& a_text)
		{
			for (auto w : kCreatureWords) {
				if (Contains(a_text, w)) {
					return true;
				}
			}
			return false;
		}

		std::string ChainOf(RE::TESIdleForm* a_idle)
		{
			std::string chain;
			int         guard = 0;
			for (auto p = a_idle->parentIdle; p && guard < 32; p = p->parentIdle, ++guard) {
				const char* id = p->GetFormEditorID();
				chain = std::string(id && *id ? id : "?") + (chain.empty() ? "" : " > ") + chain;
			}
			return chain;
		}

		void ApplyTags(Finisher& f, const std::vector<std::string>& a_tags)
		{
			bool anyWeapon = false;
			std::array<bool, kWeaponClassCount> ok{};
			for (auto t : a_tags) {
				t = Lower(t);
				if (t == "front") f.autoCategory = Category::kFront;
				else if (t == "back") f.autoCategory = Category::kBack;
				else if (t == "sneak") f.autoCategory = Category::kSneak;
				else if (t == "bleed" || t == "bleedout") f.autoCategory = Category::kBleed;
				else if (t == "a_female") f.femaleAttacker = true;
				else if (t == "a_male") f.maleAttacker = true;
				else if (t == "creature") f.creature = true;
				else if (t == "a_all" || t == "a_all_l" || t == "a_all_r") { anyWeapon = true; ok.fill(true); }
				else if (t == "a_dagger") { anyWeapon = true; ok[kDagger] = true; }
				else if (t == "a_sword") { anyWeapon = true; ok[kSword] = true; }
				else if (t == "a_axe" || t == "a_waraxe") { anyWeapon = true; ok[kAxe] = true; }
				else if (t == "a_mace") { anyWeapon = true; ok[kMace] = true; }
				else if (t == "a_dw" || t == "a_dual") { anyWeapon = true; ok[kDualWield] = true; }
				else if (t.starts_with("a_1h")) { anyWeapon = true; for (int w = kDagger; w <= kDualWield; ++w) ok[w] = true; }
				else if (t == "a_greatsword" || t == "a_2hm") { anyWeapon = true; ok[kGreatsword] = true; }
				else if (t == "a_battleaxe") { anyWeapon = true; ok[kBattleaxe] = true; }
				else if (t == "a_warhammer") { anyWeapon = true; ok[kWarhammer] = true; }
				else if (t == "a_2hw") { anyWeapon = true; ok[kBattleaxe] = ok[kWarhammer] = true; }
				else if (t.starts_with("a_2h")) { anyWeapon = true; for (int w = kGreatsword; w <= kWarhammer; ++w) ok[w] = true; }
				else if (t == "a_h2h" || t == "a_unarmed") { anyWeapon = true; ok[kUnarmed] = true; }
				else if (t == "a_bow" || t == "a_ranged") { anyWeapon = true; ok[kRanged] = true; }
			}
			f.killmove = true;
			if (anyWeapon) {
				f.hasWeaponTags = true;
				f.weaponOk = ok;
			}
		}
	}

	const char* CategoryName(Category a_cat)
	{
		switch (a_cat) {
		case Category::kFront: return "Front";
		case Category::kBack: return "Back";
		case Category::kSneak: return "Sneak";
		case Category::kBleed: return "Bleedout";
		case Category::kAny: return "Any";
		default: return "?";
		}
	}

	const char* WeaponClassName(int a_class)
	{
		static const char* n[] = { "Unarmed", "Dagger", "Sword", "Axe", "Mace", "Dual wield", "Greatsword", "Battleaxe", "Warhammer", "Ranged", "Other" };
		return a_class >= 0 && a_class < kWeaponClassCount ? n[a_class] : "?";
	}

	Finisher* Catalog::Find(const std::string& a_key)
	{
		for (auto& f : _list) {
			if (f.key == a_key) {
				return &f;
			}
		}
		return nullptr;
	}

	void Catalog::Build()
	{
		_list.clear();
		auto data = RE::TESDataHandler::GetSingleton();
		if (!data) {
			return;
		}
		std::unordered_map<std::string, std::size_t> index;
		std::unordered_map<std::string, std::string> idleEventByEditorID;  // lower EDID -> key

		// Idle forms are not kept in the data handler's per-type arrays, so walk the global form map
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
		logger::info("Scanning {} idle forms", idles.size());
		for (auto idle : idles) {
			std::string ev = idle->animEventName.c_str() ? idle->animEventName.c_str() : "";
			std::string key = Lower(ev);
			if (!key.starts_with("pa_")) {
				continue;
			}
			auto it = index.find(key);
			if (it == index.end()) {
				Finisher f;
				f.event = ev;
				f.key = key;
				f.chain = ChainOf(idle);
				const char* id = idle->GetFormEditorID();
				f.editorID = id ? id : "";
				index[key] = _list.size();
				_list.push_back(std::move(f));
				it = index.find(key);
			}
			auto& f = _list[it->second];
			f.idles.push_back(idle);
			if (auto file = idle->GetFile(0)) {
				f.sources.insert(std::string(file->GetFilename()));
			}
			const char* id = idle->GetFormEditorID();
			if (id && *id) {
				idleEventByEditorID[Lower(id)] = key;
			}
		}

		// heuristics from the event name and the idle tree path
		for (auto& f : _list) {
			std::string text = Lower(f.event + " " + f.chain + " " + f.editorID);
			for (auto idle : f.idles) {
				text += " " + Lower(ChainOf(idle));
			}
			f.killmove = Contains(text, "kill");
			f.creature = LooksLikeCreature(Lower(f.event + " " + f.chain));
			// a "the target is race X" condition in the idle tree means it is made for a specific race
			for (auto idle : f.idles) {
				int guard = 0;
				for (auto i = idle; i && guard < 32; i = i->parentIdle, ++guard) {
					for (auto item = i->conditions.head; item; item = item->next) {
						auto fn = item->data.functionData.function.get();
						if ((fn == RE::FUNCTION_DATA::FunctionID::kGetIsRace || fn == RE::FUNCTION_DATA::FunctionID::kGetIsID) &&
							item->data.object == RE::CONDITIONITEMOBJECT::kTarget) {
							f.targetSpecific = true;
						}
					}
				}
			}
			// weapon from the name when no tags say otherwise (h2h = unarmed, dw = dual wield...)
			{
				std::string ev = Lower(f.event + " " + f.editorID);
				std::array<bool, kWeaponClassCount> ok{};
				bool any = false;
				if (Contains(ev, "h2h") || Contains(ev, "unarmed")) { ok[kUnarmed] = true; any = true; }
				if (Contains(ev, "1hm") || Contains(ev, "1hw") || Contains(ev, "1h_")) { for (int w = kDagger; w <= kMace; ++w) ok[w] = true; any = true; }
				if (Contains(ev, "dw") || Contains(ev, "dualwield")) { ok[kDualWield] = true; any = true; }
				if (Contains(ev, "dagger") || Contains(ev, "knife")) { ok[kDagger] = true; any = true; }
				if (Contains(ev, "2hm") || Contains(ev, "2h_")) { ok[kGreatsword] = true; any = true; }
				if (Contains(ev, "2hw")) { ok[kBattleaxe] = ok[kWarhammer] = true; any = true; }
				if (Contains(ev, "bow")) { ok[kRanged] = true; any = true; }
				if (any) {
					f.hasWeaponTags = true;
					f.weaponOk = ok;
				}
			}
			for (auto w : { "falmer", "draugr" }) {
				if (Contains(Lower(f.event + " " + f.chain + " " + f.editorID), w)) {
					f.raceWord = w;
				}
			}
			if (Contains(text, "sneak")) {
				f.autoCategory = Category::kSneak;
			} else if (Contains(text, "back") || Contains(text, "behind")) {
				f.autoCategory = Category::kBack;
			} else if (Contains(text, "bleed")) {
				f.autoCategory = Category::kBleed;
			} else {
				f.autoCategory = Category::kFront;
			}
		}

		// Kaputt animation tags (Low-blow, Thicc Thighs and others ship these)
		std::error_code ec;
		if (std::filesystem::exists(kKaputtDir, ec)) {
			for (auto& e : std::filesystem::directory_iterator(kKaputtDir, ec)) {
				if (e.path().extension() != ".json") {
					continue;
				}
				try {
					std::ifstream  in(e.path());
					nlohmann::json j = nlohmann::json::parse(in, nullptr, true, true);
					for (auto& [edid, tags] : j.items()) {
						auto hit = idleEventByEditorID.find(Lower(edid));
						if (hit == idleEventByEditorID.end()) {
							hit = idleEventByEditorID.find(Lower(edid) + "00");
						}
						if (hit == idleEventByEditorID.end() || !tags.is_array()) {
							continue;
						}
						if (auto f = Find(hit->second)) {
							ApplyTags(*f, tags.get<std::vector<std::string>>());
						}
					}
					logger::info("Read Kaputt tags from {}", e.path().filename().string());
				} catch (const std::exception& ex) {
					logger::warn("Could not read {}: {}", e.path().string(), ex.what());
				}
			}
		}

		// our own event-level tags
		try {
			if (std::filesystem::exists(kMetaFile, ec)) {
				std::ifstream  in(kMetaFile);
				nlohmann::json j = nlohmann::json::parse(in, nullptr, true, true);
				if (j.contains("events")) {
					for (auto& [ev, tags] : j["events"].items()) {
						if (auto f = Find(Lower(ev)); f && tags.is_array()) {
							ApplyTags(*f, tags.get<std::vector<std::string>>());
						}
					}
				}
			}
		} catch (const std::exception& ex) {
			logger::warn("Could not read {}: {}", kMetaFile, ex.what());
		}

		// Idle-tree killmoves are refused when played directly (their tree sits under attack roots that combat
		// mods rebuild), while stand-alone idles such as Low-blow's and Thicc's play fine. Give every idle-tree
		// killmove a stand-alone twin - same animation event, no parent, no conditions - and play that one.
		{
			auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::TESIdleForm>();
			int  made = 0;
			for (auto& f : _list) {
				if (!f.killmove || f.idles.empty() ||
					std::any_of(f.idles.begin(), f.idles.end(), [](auto i) { return i->parentIdle == nullptr; })) {
					continue;
				}
				auto src = f.idles.front();
				auto clone = factory ? factory->Create() : nullptr;
				if (!clone) {
					break;
				}
				clone->data = src->data;
				clone->data.flags = RE::IDLE_DATA::Flag::kNone;
				clone->conditions.head = nullptr;
				clone->childIdles = nullptr;
				clone->parentIdle = nullptr;
				clone->prevIdle = nullptr;
				clone->animFileName = src->animFileName;
				clone->animEventName = src->animEventName;
				f.idles.insert(f.idles.begin(), clone);
				++made;
			}
			logger::info("Made {} stand-alone twins of idle-tree killmoves", made);
		}

		std::sort(_list.begin(), _list.end(), [](const Finisher& a, const Finisher& b) {
			if (a.killmove != b.killmove) return a.killmove;
			if (a.creature != b.creature) return !a.creature;
			return a.key < b.key;
		});

		int kills = 0;
		for (auto& f : _list) {
			kills += f.killmove && !f.creature;
		}
		logger::info("Found {} paired animations ({} usable human killmoves)", _list.size(), kills);
		LoadUser();
	}

	namespace
	{
		// NiFormArray members are private in CommonLib; same layout as NiTLargeArray
		struct RawFormArray
		{
			void*          vtbl;
			RE::TESForm**  data;
			std::uint32_t  capacity;
			std::uint32_t  freeIdx;
			std::uint32_t  size;
			std::uint32_t  growBy;
		};
		static_assert(sizeof(RawFormArray) == sizeof(RE::NiFormArray));

		bool InsertChildFront(RE::TESIdleForm* a_parent, RE::TESIdleForm* a_child)
		{
			if (!a_parent->childIdles) {
				return false;
			}
			auto raw = reinterpret_cast<RawFormArray*>(a_parent->childIdles);
			std::uint32_t n = raw->size;
			auto newData = static_cast<RE::TESForm**>(RE::NiMalloc(sizeof(RE::TESForm*) * (n + 1)));
			if (!newData) {
				return false;
			}
			newData[0] = a_child;
			for (std::uint32_t i = 0; i < n; ++i) {
				newData[i + 1] = raw->data[i];
			}
			// the old buffer is left alone (never freed) in case something is iterating it right now
			raw->data = newData;
			raw->capacity = n + 1;
			raw->size = n + 1;
			raw->freeIdx = n + 1;
			return true;
		}
	}

	void Catalog::AttachOrphans()
	{
		// Stand-alone killmove idles (Low-blow, Kaputt-format packs) are not in the idle tree, so the game's own
		// killmove system can never pick them. Hang them under the vanilla killmove root, like VioLens' patches do,
		// behind a condition that is never true - it is only lifted while we let the game pick that finisher.
		std::vector<RE::TESIdleForm*> vanilla;
		for (auto& f : _list) {
			if (!f.killmove || f.creature) {
				continue;
			}
			for (auto idle : f.idles) {
				auto file = idle->GetFile(0);
				if (idle->parentIdle && file && std::string_view(file->GetFilename()) == "Skyrim.esm") {
					vanilla.push_back(idle);
				}
			}
		}
		if (vanilla.empty()) {
			logger::warn("No vanilla killmove idles found - stand-alone finishers stay out of the idle tree");
			return;
		}
		auto isAncestor = [](RE::TESIdleForm* a_node, RE::TESIdleForm* a_leaf) {
			int guard = 0;
			for (auto i = a_leaf; i && guard < 32; i = i->parentIdle, ++guard) {
				if (i == a_node) {
					return true;
				}
			}
			return false;
		};
		// The vanilla killmoves hang under a few roots (front / sneak / bleedout ...). Count how many vanilla
		// killmoves each ancestor covers and take the node covering the most - the deepest one on a tie.
		std::unordered_map<RE::TESIdleForm*, int> cover;
		std::unordered_map<RE::TESIdleForm*, int> depth;
		for (auto leaf : vanilla) {
			std::vector<RE::TESIdleForm*> chain;
			int guard = 0;
			for (auto n = leaf->parentIdle; n && guard < 32; n = n->parentIdle, ++guard) {
				chain.push_back(n);
			}
			for (std::size_t i = 0; i < chain.size(); ++i) {
				cover[chain[i]]++;
				depth[chain[i]] = static_cast<int>(chain.size() - i);  // distance from the top
			}
		}
		// only a node that is itself a killmove group: higher up are the attack trees (combat mods such as BFCO
		// put theirs there), and an idle hung there would be played as an ordinary attack
		RE::TESIdleForm* root = nullptr;
		for (auto& [node, n] : cover) {
			const char* id = node->GetFormEditorID();
			if (!id || Lower(id).find("killmove") == std::string::npos) {
				continue;
			}
			if (!root || n > cover[root] || (n == cover[root] && depth[node] > depth[root])) {
				root = node;
			}
		}
		for (auto& [node, n] : cover) {
			if (n * 4 >= static_cast<int>(vanilla.size())) {
				const char* id = node->GetFormEditorID();
				logger::info("  killmove tree node {} ({:08X}) covers {} of {} vanilla killmoves", id ? id : "?", node->GetFormID(), n, vanilla.size());
			}
		}
		(void)isAncestor;
		if (!root) {
			logger::warn("Could not find the killmove root idle");
			return;
		}
		const char* rootId = root->GetFormEditorID();
		logger::info("Killmove root idle: {} ({:08X})", rootId ? rootId : "?", root->GetFormID());

		auto playerBase = RE::TESForm::LookupByID(0x7);
		int  count = 0;
		for (auto& f : _list) {
			if (!f.killmove || f.idles.empty() ||
				std::any_of(f.idles.begin(), f.idles.end(), [](auto i) { return i->parentIdle != nullptr; })) {
				continue;
			}
			for (auto idle : f.idles) {
				auto item = new RE::TESConditionItem();
				item->data.functionData.function = RE::FUNCTION_DATA::FunctionID::kGetIsID;
				item->data.functionData.params[0] = playerBase;
				item->data.functionData.params[1] = nullptr;
				item->data.object = RE::CONDITIONITEMOBJECT::kSelf;
				item->data.flags.opCode = RE::CONDITION_ITEM_DATA::OpCode::kEqualTo;
				item->data.flags.isOR = false;
				item->data.comparisonValue.f = 2.0f;  // never true
				item->next = idle->conditions.head;
				idle->conditions.head = item;
				if (InsertChildFront(root, idle)) {
					idle->parentIdle = root;
					++count;
				}
			}
			f.chain = ChainOf(f.idles.front());
		}
		logger::info("Attached {} stand-alone finisher idles to the killmove tree", count);
	}

	void Catalog::BlockPlayerRandom()
	{
		if (_blocked || !Settings::Get().blockRandomPlayerKillmoves) {
			return;
		}
		auto playerBase = RE::TESForm::LookupByID(0x7);
		if (!playerBase) {
			return;
		}
		int count = 0;
		for (auto& f : _list) {
			if (!f.killmove) {
				continue;
			}
			for (auto idle : f.idles) {
				// only idles that live in the idle tree can be picked at random
				if (!idle->parentIdle) {
					continue;
				}
				auto item = new RE::TESConditionItem();
				item->data.functionData.function = RE::FUNCTION_DATA::FunctionID::kGetIsID;
				item->data.functionData.params[0] = playerBase;
				item->data.functionData.params[1] = nullptr;
				item->data.object = RE::CONDITIONITEMOBJECT::kSelf;
				item->data.flags.opCode = RE::CONDITION_ITEM_DATA::OpCode::kNotEqualTo;
				item->data.flags.isOR = false;
				item->data.flags.global = false;
				item->data.comparisonValue.f = 1.0f;
				item->next = idle->conditions.head;
				idle->conditions.head = item;
				++count;
			}
		}
		_blocked = true;
		logger::info("Blocked random player killmoves on {} idle forms (NPC killmoves unchanged)", count);
	}

	nlohmann::json Catalog::ToJson(bool a_stats) const
	{
		nlohmann::json j;
		j["version"] = 5;
		j["on"] = nlohmann::json::object();
		j["chance"] = nlohmann::json::object();
		j["category"] = nlohmann::json::object();
		if (a_stats) {
			j["stats"] = nlohmann::json::object();
		}
		for (auto& f : _list) {
			if (std::any_of(f.on.begin(), f.on.end(), [](bool v) { return !v; })) {
				j["on"][f.key] = std::vector<int>(f.on.begin(), f.on.end());
			}
			if (std::any_of(f.chance.begin(), f.chance.end(), [](int v) { return v != 100; })) {
				j["chance"][f.key] = std::vector<int>(f.chance.begin(), f.chance.end());
			}
			if (f.userCategory >= 0) {
				j["category"][f.key] = f.userCategory;
			}
			if (a_stats && (f.success || std::any_of(f.fails.begin(), f.fails.end(), [](int v) { return v > 0; }))) {
				j["stats"][f.key] = { { "success", f.success }, { "fail", std::vector<int>(f.fails.begin(), f.fails.end()) } };
			}
		}
		return j;
	}

	void Catalog::FromJson(const nlohmann::json& j, bool a_stats)
	{
		// a preset fully replaces the current choices
		for (auto& f : _list) {
			f.on.fill(true);
			f.chance.fill(100);
			f.userCategory = -1;
		}
		// v1 files
		if (j.contains("disabled")) {
			for (auto& k : j["disabled"]) {
				if (auto f = Find(k.get<std::string>())) {
					f->on.fill(false);
				}
			}
		}
		// files before v5 had 5 classes: Unarmed, 1H, 2H, Ranged, Other
		const bool oldClasses = j.value("version", 1) < 5;
		auto mapOld = [](int a_new) {
			if (a_new == kUnarmed) return 0;
			if (IsOneHanded(a_new)) return 1;
			if (IsTwoHanded(a_new)) return 2;
			if (a_new == kRanged) return 3;
			return 4;
		};
		if (j.contains("on")) {
			for (auto& [k, v] : j["on"].items()) {
				if (auto f = Find(k); f && v.is_array()) {
					for (int w = 0; w < kWeaponClassCount; ++w) {
						std::size_t src = oldClasses ? static_cast<std::size_t>(mapOld(w)) : static_cast<std::size_t>(w);
						if (src < v.size()) {
							f->on[w] = v[src].get<int>() != 0;
						}
					}
				}
			}
		}
		if (j.contains("chance")) {
			for (auto& [k, v] : j["chance"].items()) {
				auto f = Find(k);
				if (!f) {
					continue;
				}
				if (v.is_number()) {
					f->chance.fill(std::clamp(v.get<int>(), 0, 100));
				} else if (v.is_array()) {
					for (int w = 0; w < kWeaponClassCount; ++w) {
						std::size_t src = oldClasses ? static_cast<std::size_t>(mapOld(w)) : static_cast<std::size_t>(w);
						if (src < v.size()) {
							f->chance[w] = std::clamp(v[src].get<int>(), 0, 100);
						}
					}
				}
			}
		}
		if (j.contains("category")) {
			for (auto& [k, v] : j["category"].items()) {
				if (auto f = Find(k)) {
					f->userCategory = v.get<int>();
				}
			}
		}
		// failure counts from before v4 were made with a different way of starting finishers - start afresh
		if (a_stats && j.contains("stats") && j.value("version", 1) >= 5) {
			for (auto& [k, v] : j["stats"].items()) {
				if (auto f = Find(k)) {
					f->success = v.value("success", 0);
					if (v.contains("fail")) {
						auto arr = v["fail"].get<std::vector<int>>();
						for (std::size_t i = 0; i < arr.size() && i < f->fails.size(); ++i) {
							f->fails[i] = arr[i];
						}
					}
				}
			}
		}
	}

	void Catalog::LoadUser()
	{
		try {
			std::error_code ec;
			if (!std::filesystem::exists(kUserFile, ec)) {
				return;
			}
			std::ifstream in(kUserFile);
			FromJson(nlohmann::json::parse(in, nullptr, true, true), true);
		} catch (const std::exception& ex) {
			logger::warn("Could not read {}: {}", kUserFile, ex.what());
		}
	}

	void Catalog::SaveUser()
	{
		std::error_code ec;
		std::filesystem::create_directories(std::filesystem::path(kUserFile).parent_path(), ec);
		std::ofstream out(kUserFile);
		out << ToJson(true).dump(2);
	}

	namespace
	{
		constexpr auto kPresetDir = "Data/SKSE/Plugins/ExecutionDirector/Presets";

		std::filesystem::path PresetPath(const std::string& a_name)
		{
			std::u8string u(a_name.begin(), a_name.end());
			return std::filesystem::path(kPresetDir) / std::filesystem::path(u + u8".json");
		}

		bool ValidName(const std::string& a_name)
		{
			if (a_name.empty() || a_name.size() > 60) {
				return false;
			}
			for (char c : a_name) {
				if (std::string_view("\\/:*?\"<>|").find(c) != std::string_view::npos) {
					return false;
				}
			}
			return a_name != "." && a_name != "..";
		}
	}

	std::vector<std::string> Catalog::ListPresets() const
	{
		std::vector<std::string> out;
		std::error_code          ec;
		if (!std::filesystem::exists(kPresetDir, ec)) {
			return out;
		}
		for (auto& e : std::filesystem::directory_iterator(kPresetDir, ec)) {
			if (e.path().extension() != ".json") {
				continue;
			}
			auto u = e.path().stem().u8string();
			out.emplace_back(u.begin(), u.end());
		}
		std::sort(out.begin(), out.end());
		return out;
	}

	bool Catalog::SavePreset(const std::string& a_name)
	{
		if (!ValidName(a_name)) {
			return false;
		}
		try {
			std::error_code ec;
			std::filesystem::create_directories(kPresetDir, ec);
			std::ofstream out(PresetPath(a_name));
			out << ToJson(false).dump(2);
			logger::info("Saved preset {}", a_name);
			return static_cast<bool>(out);
		} catch (const std::exception& ex) {
			logger::warn("Could not save preset {}: {}", a_name, ex.what());
			return false;
		}
	}

	bool Catalog::LoadPreset(const std::string& a_name)
	{
		try {
			std::ifstream in(PresetPath(a_name));
			if (!in) {
				return false;
			}
			FromJson(nlohmann::json::parse(in, nullptr, true, true), false);
			SaveUser();
			logger::info("Loaded preset {}", a_name);
			return true;
		} catch (const std::exception& ex) {
			logger::warn("Could not load preset {}: {}", a_name, ex.what());
			return false;
		}
	}

	bool Catalog::DeletePreset(const std::string& a_name)
	{
		std::error_code ec;
		return std::filesystem::remove(PresetPath(a_name), ec);
	}
}
