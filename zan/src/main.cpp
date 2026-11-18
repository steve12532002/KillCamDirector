#include "PCH.h"

#include "Core.h"
#include "Input.h"
#include "Settings.h"

namespace Zan::UI { void Register(); }

namespace
{
	void SetupLog()
	{
		auto path = logger::log_directory();
		if (!path) {
			return;
		}
		*path /= "Zan.log";
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kPreLoadGame:
			Zan::Core::Get().OnPreLoad();
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			Zan::Core::Get().Build();
			Zan::Input::Install();
			Zan::UI::Register();
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	SetupLog();
	logger::info("Zan loading");
	Zan::Settings::Get().Load();
	Zan::Core::InstallHooks();
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
