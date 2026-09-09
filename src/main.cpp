#include "pch.h"
#include "config.h"
#include "corpse_finder.h"
#include "esp_renderer.h"
#include "quickloot_compat/quickloot_compat.h"
#include "searched_corpses.h"
#include "ui_menu.h"

namespace
{
    void initialize_log()
    {
        std::optional<std::filesystem::path> path = logger::log_directory();
        if (!path)
            util::report_and_fail("Failed to find standard logging directory"sv);

        *path /= fmt::format("{}.log"sv, Plugin::NAME);
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);

        constexpr static spdlog::level::level_enum Level = spdlog::level::info;

        auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
        log->set_level(Level);
        log->flush_on(Level);

        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("%g(%#): [%^%l%$] %v"s);
    }

    void message_handler(SKSE::MessagingInterface::Message* a_msg)
    {
        switch (a_msg->type)
        {
        case SKSE::MessagingInterface::kDataLoaded:
        case SKSE::MessagingInterface::kNewGame:
        case SKSE::MessagingInterface::kPostLoadGame:
            // 此时渲染器已初始化完毕，交换链已存在，可以安全安装 Present 钩子
            ESPRenderer::install();
            // 所有插件已加载完毕：注册 MCP 参数面板（探测 SKSEMenuFramework.dll）
            UiMenu::register_menus();
            // 注册库存评估缓存的失效监听（幂等），并清空缓存（读档/新游戏旧评估作废）
            CorpseFinder::reset_loot_cache();
            break;
        case SKSE::MessagingInterface::kSaveGame:
            // 玩家存档时落盘 MCP 菜单的未保存改动（SKSE 无 shutdown 消息，
            // 不用 atexit——CRT 退出阶段引擎状态已拆解，时机不安全）
            Config::save_if_dirty();
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

    REL::Version const runtime = a_skse->RuntimeVersion();
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
    v.AuthorName("AmazingBuff");
    v.UsesAddressLibrary();
    v.UsesNoStructs();
    v.CompatibleVersions({ SKSE::RUNTIME_SSE_LATEST });

    return v;
}();

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(SKSE::LoadInterface const* a_skse)
{
    REL::Module::reset();  // Clib-NG bug workaround

    initialize_log();
    logger::info("{} v{}"sv, Plugin::NAME, Plugin::VERSION.string());

    SKSE::Init(a_skse);

    Config::load();
    // 注册已搜索尸体的 co-save 序列化回调（Save/Load/Revert/FormDelete）
    SearchedCorpses::register_serialization_callbacks();
    // QuickLoot IE 兼容（可选依赖）：拿取路径不产生激活事件，改经其公开 API
    // 把"打开战利品菜单"计入已搜索标记；未安装时静默降级
    (void)QuickLootCompat::install();

    SKSE::GetMessagingInterface()->RegisterListener(message_handler);

    logger::info("{} loaded"sv, Plugin::NAME);
    return true;
}
