#include "PCH.h"
#include "Director.h"

#include <MinHook.h>

namespace KCD
{
	namespace
	{
		using Vec = RE::NiPoint3;

		constexpr float kPi = 3.14159265358979f;

		float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
		float Lerp(float a, float b, float t) { return a + (b - a) * t; }
		float Smooth(float t)
		{
			t = Clamp01(t);
			return t * t * (3 - 2 * t);
		}
		float Damp(float a_dt, float a_tau) { return a_tau <= 0.0001f ? 1.0f : 1.0f - std::exp(-a_dt / a_tau); }

		Vec Normalized(Vec v, Vec a_fallback)
		{
			float l = v.Length();
			return l > 1e-4f ? v / l : a_fallback;
		}

		Vec RotZ(const Vec& v, float a)
		{
			float c = std::cos(a), s = std::sin(a);
			return { v.x * c - v.y * s, v.x * s + v.y * c, v.z };
		}

		Vec Column(const RE::NiMatrix3& m, int c) { return { m.entry[0][c], m.entry[1][c], m.entry[2][c] }; }

		Vec Focus(RE::Actor* a_actor)
		{
			static const RE::BSFixedString names[] = { RE::BSFixedString("NPC Spine2 [Spn2]"), RE::BSFixedString("NPC Spine1 [Spn1]"), RE::BSFixedString("NPC COM [COM ]") };
			if (auto root = a_actor->Get3D(false)) {
				for (auto& n : names) {
					if (auto node = root->GetObjectByName(n)) {
						return node->world.translate;
					}
				}
			}
			auto p = a_actor->GetPosition();
			p.z += 90.0f;
			return p;
		}

		void Notify(const char* a_msg)
		{
			using func_t = void (*)(const char*, const char*, bool);
			static REL::Relocation<func_t> func{ RELOCATION_ID(52050, 52933) };
			func(a_msg, nullptr, true);
		}

		// ---------------------------------------------------------------- anim events
		class AnimSink : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
		{
		public:
			static AnimSink* Get()
			{
				static AnimSink s;
				return &s;
			}
			RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
			{
				if (a_event && a_event->tag.c_str()) {
					Director::Get().OnAnimEvent(a_event->tag.c_str());
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// ---------------------------------------------------------------- camera hook
		using UpdateFn = void (*)(RE::PlayerCamera*);
		UpdateFn g_origUpdate = nullptr;

		void HookedUpdate(RE::PlayerCamera* a_this)
		{
			g_origUpdate(a_this);
			Director::Get().OnCameraUpdate(a_this);
		}
	}

	void Director::InstallHooks()
	{
		REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(49852, 50784) };
		if (MH_Initialize() != MH_OK) {
			logger::error("MinHook init failed");
			return;
		}
		auto addr = reinterpret_cast<LPVOID>(target.address());
		if (MH_CreateHook(addr, reinterpret_cast<LPVOID>(&HookedUpdate), reinterpret_cast<LPVOID*>(&g_origUpdate)) != MH_OK ||
			MH_EnableHook(addr) != MH_OK) {
			logger::error("Failed to hook PlayerCamera::Update");
			return;
		}
		logger::info("Hooked PlayerCamera::Update at {:X}", target.address());
	}

	void Director::OnAnimEvent(std::string_view a_tag)
	{
		if (!_active) {
			return;
		}
		std::string t{ a_tag };
		std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		if (t.find("hitframe") != std::string::npos || t.find("killactor") != std::string::npos ||
			t.find("decapitate") != std::string::npos) {
			_hitTriggers.fetch_add(1);
			if (Settings::Get().verbose) {
				logger::info("anim event {}", a_tag);
			}
		}
	}

	RE::Actor* Director::FindPartner(RE::PlayerCharacter* a_player) const
	{
		RE::Actor* best = nullptr;
		float      maxD = Settings::Get().maxPartnerDistance;
		float      bestD = maxD * maxD;
		auto       pos = a_player->GetPosition();
		if (auto lists = RE::ProcessLists::GetSingleton()) {
			lists->ForEachHighActor([&](RE::Actor* a) {
				if (a && a != a_player && a->IsInKillMove()) {
					float d = a->GetPosition().GetSquaredDistance(pos);
					if (d < bestD) {
						bestD = d;
						best = a;
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		}
		return best;
	}

	void Director::SetTimeMultiplier(float a_mult)
	{
		if (auto timer = RE::BSTimer::GetSingleton()) {
			timer->SetGlobalTimeMultiplier(a_mult, true);
		}
		_timeMult = a_mult;
	}

	void Director::OnCameraUpdate(RE::PlayerCamera* a_camera)
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		auto& s = Settings::Get();
		if (!player || !a_camera || !a_camera->cameraRoot) {
			return;
		}
		auto  timer = RE::BSTimer::GetSingleton();
		float dt = timer ? timer->realTimeDelta : (1.0f / 60.0f);
		float gdt = timer ? timer->delta : dt;
		dt = std::clamp(dt, 0.0f, 0.1f);
		gdt = std::clamp(gdt, 0.0f, 0.1f);

		auto ui = RE::UI::GetSingleton();
		if (ui && ui->GameIsPaused()) {
			if (_active) {
				ApplyCamera(a_camera, { _lastPos.x, _lastPos.y, _lastPos.z }, _lastRot);
			}
			return;
		}

		bool want = s.enabled && player->IsInKillMove();
		if (want && !_active) {
			Start(player, a_camera);
		} else if (!want && _active) {
			Stop(a_camera);
		}

		if (_active) {
			UpdateActive(player, a_camera, dt, gdt);
			return;
		}

		// hand the camera back smoothly
		if (_outTimer > 0.0f && s.blendOut > 0.0f) {
			auto&  w = a_camera->cameraRoot->world;
			float  t = Smooth(1.0f - _outTimer / s.blendOut);
			Vec    vp = w.translate;
			Vec    lp{ _lastPos.x, _lastPos.y, _lastPos.z };
			Vec    pos = lp + (vp - lp) * t;
			// blend forward vectors, rebuild
			Vec f0 = Column(_lastRot, _fwdCol) * static_cast<float>(_fwdSign);
			Vec f1 = Column(w.rotate, _fwdCol) * static_cast<float>(_fwdSign);
			Vec f = Normalized(f0 + (f1 - f0) * t, f1);
			ApplyCamera(a_camera, { pos.x, pos.y, pos.z }, LookRotation({ f.x, f.y, f.z }));
			_outTimer -= dt;
		} else {
			DetectAxes(player, a_camera);
		}
		UpdateLetterbox(dt, 0.0f);
		UpdateSlowMotion(dt);
	}

	void Director::Start(RE::PlayerCharacter* a_player, RE::PlayerCamera* a_camera)
	{
		auto& s = Settings::Get();
		_preset = s.PickPreset();
		static Preset fallback = [] {
			Preset p;
			p.name = "Built-in";
			p.shots.push_back(Shot{});
			return p;
		}();
		if (!_preset) {
			_preset = &fallback;
		}
		_active = true;
		_time = _shotTime = 0;
		_shot = 0;
		_hitTriggers = 0;
		_slowTimer = 0;
		_outTimer = 0;
		_partner.reset();
		if (auto partner = FindPartner(a_player)) {
			_partner = partner->GetHandle();
			partner->AddAnimationGraphEventSink(AnimSink::Get());
		}
		a_player->AddAnimationGraphEventSink(AnimSink::Get());

		_forcedThird = false;
		if (s.forceThirdPerson && a_camera->IsInFirstPerson()) {
			a_camera->ForceThirdPerson();
			_forcedThird = true;
		}
		_savedFOV = a_camera->GetRuntimeData2().worldFOV;

		// start the first shot from wherever the game camera is now
		auto& w = a_camera->cameraRoot->world;
		Vec   f = Column(w.rotate, _fwdCol) * static_cast<float>(_fwdSign);
		_camPos = { w.translate.x, w.translate.y, w.translate.z };
		_camTarget = { w.translate.x + f.x * 150.0f, w.translate.y + f.y * 150.0f, w.translate.z + f.z * 150.0f };

		auto& shot = _preset->shots.front();
		_side = shot.side == 1 ? 1 : shot.side == -1 ? -1 : (std::uniform_int_distribution<int>(0, 1)(_rng) ? 1 : -1);
		_snap = shot.blend <= 0.0f;
		_blendLen = _blendTimer = shot.blend;
		_axisInit = false;
		logger::info("Killmove camera start: preset '{}', partner {}", _preset->name, _partner ? "found" : "none");
	}

	void Director::Stop(RE::PlayerCamera* a_camera)
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		if (player) {
			player->RemoveAnimationGraphEventSink(AnimSink::Get());
		}
		if (auto partner = _partner.get()) {
			partner->RemoveAnimationGraphEventSink(AnimSink::Get());
		}
		_partner.reset();
		if (_savedFOV > 0.0f) {
			a_camera->GetRuntimeData2().worldFOV = _savedFOV;
		}
		_active = false;
		_outTimer = Settings::Get().blendOut;
		if (_forcedThird) {
			a_camera->ForceFirstPerson();
			_forcedThird = false;
			_outTimer = 0;
		}
		logger::info("Killmove camera end");
	}

	void Director::UpdateActive(RE::PlayerCharacter* a_player, RE::PlayerCamera* a_camera, float a_dt, float a_gdt)
	{
		auto& s = Settings::Get();
		auto& shots = _preset->shots;

		_time += a_gdt;
		_shotTime += a_gdt;
		while (_shot + 1 < shots.size() && _shotTime >= shots[_shot].duration) {
			_shotTime -= shots[_shot].duration;
			++_shot;
			auto& ns = shots[_shot];
			if (ns.side == 1 || ns.side == -1) {
				_side = ns.side;
			} else if (ns.side == 0) {
				_side = std::uniform_int_distribution<int>(0, 1)(_rng) ? 1 : -1;
			}
			_snap = ns.blend <= 0.0f;
			_blendLen = _blendTimer = ns.blend;
			_axisInit = false;
		}
		auto& shot = shots[_shot];

		// late partner pick-up (the victim's flag can lag a frame)
		RE::NiPointer<RE::Actor> partner = _partner.get();
		if (!partner && _time < 1.0f) {
			if (auto p = FindPartner(a_player)) {
				_partner = p->GetHandle();
				p->AddAnimationGraphEventSink(AnimSink::Get());
				partner = _partner.get();
			}
		}

		Vec P = Focus(a_player);
		float heading = a_player->GetAngleZ();
		Vec   fwd{ std::sin(heading), std::cos(heading), 0 };
		Vec   O = partner ? Focus(partner.get()) : P + fwd * 120.0f;

		Vec rawAxis = Normalized({ O.x - P.x, O.y - P.y, 0 }, fwd);
		Vec axis{ _axis.x, _axis.y, 0 };
		if (!_axisInit) {
			axis = rawAxis;
			_axisInit = true;
		} else if (axis.Dot(rawAxis) > -0.3f) {
			axis = Normalized(axis + (rawAxis - axis) * Damp(a_dt, 2.5f), rawAxis);
		}
		_axis = { axis.x, axis.y, 0 };

		float t = shot.duration > 0 ? Smooth(_shotTime / shot.duration) : 1.0f;
		float yaw = Lerp(shot.yaw0, shot.yaw1, t) * kPi / 180.0f * static_cast<float>(_side);
		float dist = Lerp(shot.dist0, shot.dist1, t);
		float height = Lerp(shot.height0, shot.height1, t);
		float fov = Lerp(shot.fov0, shot.fov1, t);

		Vec mid = (P + O) * 0.5f;
		Vec dir = RotZ(axis * -1.0f, yaw);
		Vec wantPos = mid + dir * dist + Vec{ 0, 0, height };
		Vec wantTarget = P + (O - P) * Clamp01(shot.lookAt);

		Vec curPos{ _camPos.x, _camPos.y, _camPos.z };
		Vec curTarget{ _camTarget.x, _camTarget.y, _camTarget.z };
		if (_snap) {
			curPos = wantPos;
			curTarget = wantTarget;
			_snap = false;
		} else {
			float tau = s.smoothing;
			if (_blendTimer > 0.0f) {
				tau = std::max(tau, _blendLen * 0.35f);
				_blendTimer -= a_dt;
			}
			float k = Damp(a_dt, tau);
			curPos = curPos + (wantPos - curPos) * k;
			curTarget = curTarget + (wantTarget - curTarget) * Damp(a_dt, tau * 0.7f);
		}
		_camPos = { curPos.x, curPos.y, curPos.z };
		_camTarget = { curTarget.x, curTarget.y, curTarget.z };

		Vec f = Normalized(curTarget - curPos, fwd);
		auto rot = LookRotation({ f.x, f.y, f.z });
		ApplyCamera(a_camera, _camPos, rot);
		_lastPos = _camPos;
		_lastRot = rot;

		if (fov > 1.0f) {
			a_camera->GetRuntimeData2().worldFOV = fov;
		}

		float lb = s.letterboxOverride >= 0.0f ? s.letterboxOverride : _preset->letterbox;
		UpdateLetterbox(a_dt, lb);
		UpdateSlowMotion(a_dt);
	}

	void Director::UpdateSlowMotion(float a_dt)
	{
		auto& s = Settings::Get();
		bool  want = false;
		float scale = 1.0f;
		if (_active && _preset) {
			bool on = s.slowMotionOverride >= 0 ? s.slowMotionOverride == 1 : _preset->slowMotion;
			scale = s.slowScaleOverride > 0.0f ? s.slowScaleOverride : _preset->slowScale;
			int trig = _hitTriggers.exchange(0);
			if (on) {
				if (trig > 0 && _preset->slowOnHit) {
					_slowTimer = _preset->slowDuration;
				}
				if (_preset->slowStart >= 0.0f && _time >= _preset->slowStart && _time <= _preset->slowEnd) {
					want = true;
				}
				if (!_preset->slowOnHit && _preset->slowStart < 0.0f) {
					want = true;
				}
				if (_slowTimer > 0.0f) {
					want = true;
					_slowTimer -= a_dt;
				}
			}
		}
		float target = want ? std::clamp(scale, 0.05f, 1.0f) : 1.0f;
		if (!_ownsTimeMult && target == 1.0f) {
			return;
		}
		float m = _timeMult + (target - _timeMult) * Damp(a_dt, 0.08f);
		if (std::abs(m - target) < 0.01f) {
			m = target;
		}
		SetTimeMultiplier(m);
		_ownsTimeMult = m != 1.0f;
	}

	void Director::UpdateLetterbox(float a_dt, float a_target)
	{
		a_target = std::clamp(a_target, 0.0f, 0.25f);
		float cur = _letterbox.load();
		cur = cur + (a_target - cur) * Damp(a_dt, 0.15f);
		if (std::abs(cur - a_target) < 0.001f) {
			cur = a_target;
		}
		_letterbox.store(cur);
	}

	void Director::DetectAxes(RE::PlayerCharacter* a_player, RE::PlayerCamera* a_camera)
	{
		auto& s = Settings::Get();
		if (s.axisMode == 0 || s.axisMode == 1) {
			_fwdCol = s.axisMode == 0 ? 1 : 0;
			_fwdSign = 1;
			_upCol = s.axisMode == 0 ? 2 : 1;
			_upSign = 1;
			return;
		}
		if (_axesLocked || !a_camera->IsInThirdPerson()) {
			return;
		}
		auto& w = a_camera->cameraRoot->world;
		Vec   toPlayer = Focus(a_player) - w.translate;
		if (toPlayer.Length() < 60.0f) {
			return;
		}
		toPlayer = Normalized(toPlayer, { 0, 1, 0 });
		int   fc = -1, fs = 1;
		float best = 0;
		for (int c = 0; c < 3; ++c) {
			float d = Column(w.rotate, c).Dot(toPlayer);
			if (std::abs(d) > best) {
				best = std::abs(d);
				fc = c;
				fs = d > 0 ? 1 : -1;
			}
		}
		if (fc < 0 || best < 0.85f) {
			_detectStreak = 0;
			return;
		}
		int   uc = -1, us = 1;
		float bestU = 0;
		for (int c = 0; c < 3; ++c) {
			if (c == fc) {
				continue;
			}
			float d = Column(w.rotate, c).z;
			if (std::abs(d) > bestU) {
				bestU = std::abs(d);
				uc = c;
				us = d > 0 ? 1 : -1;
			}
		}
		if (uc < 0 || bestU < 0.6f) {
			_detectStreak = 0;
			return;
		}
		if (fc == _fwdCol && fs == _fwdSign && uc == _upCol && us == _upSign) {
			if (++_detectStreak >= 45) {
				_axesLocked = true;
				logger::info("Camera axes detected: forward col {} sign {}, up col {} sign {}", fc, fs, uc, us);
			}
		} else {
			_fwdCol = fc;
			_fwdSign = fs;
			_upCol = uc;
			_upSign = us;
			_detectStreak = 0;
		}
	}

	RE::NiMatrix3 Director::LookRotation(const Vec3& a_forward) const
	{
		Vec f = Normalized({ a_forward.x, a_forward.y, a_forward.z }, { 0, 1, 0 });
		Vec up{ 0, 0, 1 };
		Vec r = f.Cross(up);
		if (r.Length() < 1e-3f) {
			r = { 1, 0, 0 };
		}
		r = Normalized(r, { 1, 0, 0 });
		Vec u = r.Cross(f);

		Vec cols[3];
		cols[_fwdCol] = f * static_cast<float>(_fwdSign);
		cols[_upCol] = u * static_cast<float>(_upSign);
		int rc = 3 - _fwdCol - _upCol;
		cols[rc] = cols[(rc + 1) % 3].Cross(cols[(rc + 2) % 3]);

		RE::NiMatrix3 m;
		for (int c = 0; c < 3; ++c) {
			m.entry[0][c] = cols[c].x;
			m.entry[1][c] = cols[c].y;
			m.entry[2][c] = cols[c].z;
		}
		return m;
	}

	void Director::ApplyCamera(RE::PlayerCamera* a_camera, const Vec3& a_pos, const RE::NiMatrix3& a_rot)
	{
		auto root = a_camera->cameraRoot.get();
		if (!root) {
			return;
		}
		Vec wp{ a_pos.x, a_pos.y, a_pos.z };
		if (auto parent = root->parent) {
			auto&         pw = parent->world;
			RE::NiMatrix3 inv = pw.rotate.Transpose();
			float         sc = pw.scale != 0.0f ? pw.scale : 1.0f;
			root->local.translate = (inv * (wp - pw.translate)) / sc;
			root->local.rotate = inv * a_rot;
		} else {
			root->local.translate = wp;
			root->local.rotate = a_rot;
		}
		RE::NiUpdateData ud{};
		root->Update(ud);
	}

	void Director::NotifyText(const char* a_msg)
	{
		if (Settings::Get().notifications) {
			Notify(a_msg);
		}
	}
}
