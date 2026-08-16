#include "PCH.h"
#include "Config.h"
#include "CorpseFinder.h"
#include "ESPRenderer.h"

namespace
{
    void InitializeLog()
    {
        auto path = logger::log_directory();
        if (!path)
        {
            util::report_and_fail("Failed to find standard logging directory"sv);
        }

        *path /= fmt::format("{}.log"sv, Plugin::NAME);
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);

        constexpr auto level = spdlog::level::info;

        auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
        log->set_level(level);
        log->flush_on(level);

        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("%g(%#): [%^%l%$] %v"s);
    }

    void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
    {
        switch (a_msg->type)
        {
        case SKSE::MessagingInterface::kDataLoaded:
        case SKSE::MessagingInterface::kNewGame:
        case SKSE::MessagingInterface::kPostLoadGame:
            // 此时渲染器已初始化完毕，交换链已存在，可以安全安装 Present 钩子
            ESPRenderer::Install();
            break;
        default:
            break;
        }
    }
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Query(SKSE::QueryInterface const* a_skse, SKSE::PluginInfo* a_info)
{
    a_info->infoVersion = SKSE::PluginInfo::kVersion;
    a_info->name = Plugin::NAME.data();
    a_info->version = Plugin::VERSION[0];

    if (a_skse->IsEditor())
    {
        logger::critical("Loaded in editor, marking as incompatible"sv);
        return false;
    }

    auto const runtime = a_skse->RuntimeVersion();
    if (runtime < SKSE::RUNTIME_SSE_1_6_629)
    {
        logger::critical("Unsupported runtime version {}"sv, runtime.string());
        return false;
    }

    return true;
}

// SKSE 2.2.x 要求插件导出 SKSEPlugin_Version 版本数据；缺少该导出会在
// "checking plugin" 阶段被跳过（日志出现 "no version data (handle 0)"）
extern "C" DLLEXPORT constinit auto SKSEPlugin_Version = [] {
    SKSE::PluginVersionData v;

    v.PluginVersion(Plugin::VERSION);
    v.PluginName(Plugin::NAME);
    v.AuthorName("CorpseESP");
    v.UsesAddressLibrary();
    v.UsesNoStructs();
    v.CompatibleVersions({ SKSE::RUNTIME_SSE_LATEST });

    return v;
}();

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(SKSE::LoadInterface const* a_skse)
{
    REL::Module::reset();  // Clib-NG bug workaround

    InitializeLog();
    logger::info("{} v{}"sv, Plugin::NAME, Plugin::VERSION.string());

    SKSE::Init(a_skse);

    Config::Load();

    SKSE::GetMessagingInterface()->RegisterListener(MessageHandler);

    logger::info("{} loaded"sv, Plugin::NAME);
    return true;
}
