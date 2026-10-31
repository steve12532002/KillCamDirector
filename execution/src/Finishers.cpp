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
			"ashhopper", "creature", "frostbite", "atronach", "wisp" };

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
				else if (t.starts_with("a_1h") || t == "a_dagger" || t == "a_sword" || t == "a_axe" || t == "a_mace") { anyWeapon = true; ok[kOneHand] = true; }
				else if (t.starts_with("a_2h")) { anyWeapon = true; ok[kTwoHand] = true; }
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
		static const char* n[] = { "Unarmed", "1H", "2H", "Ranged", "Other" };
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
		j["version"] = 2;
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
		if (j.contains("on")) {
			for (auto& [k, v] : j["on"].items()) {
				if (auto f = Find(k); f && v.is_array()) {
					for (std::size_t i = 0; i < v.size() && i < f->on.size(); ++i) {
						f->on[i] = v[i].get<int>() != 0;
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
					for (std::size_t i = 0; i < v.size() && i < f->chance.size(); ++i) {
						f->chance[i] = std::clamp(v[i].get<int>(), 0, 100);
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
		if (a_stats && j.contains("stats")) {
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
