#include "config/config.h"
#include "quickloot_compat/quickloot_compat.h"
#include "render/esp_renderer.h"
#include "search/searched_corpses.h"
#include "ui/ui_menu.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>

namespace
{
    constexpr spdlog::level::level_enum Log_Level = spdlog::level::info;
    void initialize_log()
    {
        std::optional<std::filesystem::path> path = logger::log_directory();
        if (!path)
            SKSE::stl::report_and_fail("Failed to find standard logging directory"sv);

        *path /= fmt::format("{}.log"sv, Plugin::Plugin_Name);
        std::shared_ptr<spdlog::sinks::basic_file_sink_mt> sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
        std::shared_ptr<spdlog::logger> log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));

        log->set_level(Log_Level);
        log->flush_on(Log_Level);

        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("%g(%#): [%^%l%$] %v"s);
    }

    void message_handler(SKSE::MessagingInterface::Message* a_msg)
    {
        switch (a_msg->type)
        {
        case SKSE::MessagingInterface::kDataLoaded:
            (void)PLUGIN_NAMESPACE::QuickLootCompat::install();
        case SKSE::MessagingInterface::kPostLoadGame:
            PLUGIN_NAMESPACE::Renderer::install();
            PLUGIN_NAMESPACE::Menu::register_menu();
            break;
        case SKSE::MessagingInterface::kSaveGame:
            PLUGIN_NAMESPACE::Setting::save();
            break;
        default:
            break;
        }
    }
}

extern "C" DLLEXPORT bool SKSEPlugin_Load(SKSE::LoadInterface const* a_skse)
{
    REL::Module::reset();  // Clib-NG bug workaround

    initialize_log();
    logger::info("{} v{}"sv, Plugin::Plugin_Name, Plugin::Plugin_Version.string());

    SKSE::Init(a_skse);

    PLUGIN_NAMESPACE::Setting::load();
    PLUGIN_NAMESPACE::MarkCorpse::install();

    SKSE::GetMessagingInterface()->RegisterListener(message_handler);

    logger::info("{} loaded"sv, Plugin::Plugin_Name);
    return true;
}

extern "C" DLLEXPORT constinit auto SKSEPlugin_Version = [] {
    SKSE::PluginVersionData v;
    v.PluginVersion(Plugin::Plugin_Version);
    v.PluginName(Plugin::Plugin_Name);
    v.AuthorName(Plugin::Plugin_Author);
    v.UsesAddressLibrary();
    v.UsesNoStructs();
    return v;
}();

extern "C" DLLEXPORT bool SKSEPlugin_Query(SKSE::QueryInterface const*, SKSE::PluginInfo* a_info)
{
    a_info->infoVersion = SKSE::PluginInfo::kVersion;
    a_info->name = SKSEPlugin_Version.pluginName;
    a_info->version = SKSEPlugin_Version.pluginVersion;
    return true;
}