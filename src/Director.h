#pragma once

#include "Settings.h"

namespace KCD
{
	class Director
	{
	public:
		static Director& Get()
		{
			static Director d;
			return d;
		}

		static void InstallHooks();

		void OnCameraUpdate(RE::PlayerCamera* a_camera);
		void OnAnimEvent(std::string_view a_tag);

		// UI helpers
		float CurrentLetterbox() const { return _letterbox.load(); }
		static void NotifyText(const char* a_msg);
		bool  IsActive() const { return _active; }
		const Preset* CurrentPreset() const { return _preset; }

	private:
		struct Vec3
		{
			float x{ 0 }, y{ 0 }, z{ 0 };
		};

		void Start(RE::PlayerCharacter* a_player, RE::PlayerCamera* a_camera);
		void Stop(RE::PlayerCamera* a_camera);
		void UpdateActive(RE::PlayerCharacter* a_player, RE::PlayerCamera* a_camera, float a_dt, float a_gdt);
		void UpdateSlowMotion(float a_dt);
		void UpdateLetterbox(float a_dt, float a_target);
		void DetectAxes(RE::PlayerCharacter* a_player, RE::PlayerCamera* a_camera);
		void ApplyCamera(RE::PlayerCamera* a_camera, const Vec3& a_pos, const RE::NiMatrix3& a_rot);
		RE::NiMatrix3 LookRotation(const Vec3& a_forward) const;
		RE::Actor* FindPartner(RE::PlayerCharacter* a_player) const;
		bool       WantsCamera(RE::PlayerCharacter* a_player) const;
		void SetTimeMultiplier(float a_mult);

		bool          _active{ false };
		const Preset* _preset{ nullptr };
		RE::ActorHandle _partner;
		float _time{ 0 };       // game time since start
		float _shotTime{ 0 };
		std::size_t _shot{ 0 };
		int   _side{ 1 };
		float _blendTimer{ 0 };  // remaining blend time for current shot change
		float _blendLen{ 0 };
		bool  _snap{ true };
		Vec3  _axis{ 0, 1, 0 };
		Vec3  _facing{ 0, 1, 0 };
		bool  _facingInit{ false };
		float _offTimer{ 0 };
		float _sinceStop{ 1000.0f };
		bool  _sidePending{ false };
		float _zoom{ 1.0f };
		bool  _tight{ false };
		bool  _needFraming{ true };
		Vec3  _tightDir{ 0, -1, 0 };
		float _tightDist{ 150 };
		float _pullIn{ 1.0f };
		bool  _axisInit{ false };
		Vec3  _camPos;
		Vec3  _camTarget;
		bool  _forcedThird{ false };
		float _savedFOV{ -1 };

		// hand-back
		float         _outTimer{ 0 };
		Vec3          _lastPos;
		RE::NiMatrix3 _lastRot;

		// slow motion
		std::atomic<int> _hitTriggers{ 0 };
		float _slowTimer{ 0 };
		float _timeMult{ 1.0f };
		bool  _ownsTimeMult{ false };

		// letterbox (0..0.25)
		std::atomic<float> _letterbox{ 0 };

		// axis detection: column index & sign for forward and up
		int  _fwdCol{ 1 }, _fwdSign{ 1 }, _upCol{ 2 }, _upSign{ 1 };
		int  _detectStreak{ 0 };
		bool _axesLocked{ false };

		std::mt19937 _rng{ std::random_device{}() };
	};
}
