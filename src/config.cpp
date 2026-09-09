#include "pch.h"
#include "config.h"

#include <fstream>

namespace
{
    Config::Settings g_settings;
    std::atomic<bool> g_enabled{ true };
    bool g_dirty = false;  // MCP 菜单改动标记（游戏线程读写：菜单回调与 kSaveGame 消息都在游戏线程）
}

namespace Config
{
    std::filesystem::path get_ini_path() noexcept
    {
        // 放在游戏 Data\SKSE\Plugins\ 下，与 DLL 同目录
        REL::stl::zwstring const exePath = REL::Module::get().filePath();  // SkyrimSE.exe 的完整路径（wstring_view）
        const std::filesystem::path game_root(exePath);
        return game_root.parent_path() / "Data" / "SKSE" / "Plugins" / (std::string(Plugin::NAME) + ".ini");
    }

    namespace
    {
        std::uint32_t parse_hex(char const* a_value, std::uint32_t a_default) noexcept
        {
            if (!a_value || !*a_value)
                return a_default;

            char* end = nullptr;
            uint32_t const value = std::strtoul(a_value, &end, 16);
            return end == a_value ? a_default : value;
        }

        // INI 是本插件唯一的外部信任边界：所有取值在此一次性规范化到有效域，
        // 之后扫描/渲染/菜单都按这些前置条件工作，不再重复校验。
        // 关键约束：max_distance <= 0 会让 TES::ForEachReferenceInRange 退化为全量遍历；
        // min_opacity > 1 会让渲染端 std::clamp 的 lo > hi（未定义行为）。
        void sanitize(Settings& a_settings) noexcept
        {
            a_settings.hotkey = a_settings.hotkey > 0xFEu ? 0u : a_settings.hotkey;  // 0 = 不绑定
            a_settings.max_distance = std::clamp(a_settings.max_distance, 100.0f, 100000.0f);
            a_settings.scan_interval_ms = std::clamp(a_settings.scan_interval_ms, 50u, 60000u);
            a_settings.outline_color &= 0x00FFFFFFu;
            a_settings.min_opacity = std::clamp(a_settings.min_opacity, 0.0f, 1.0f);
            a_settings.outline_thickness = std::clamp(a_settings.outline_thickness, 1.0f, 16.0f);
            a_settings.fade_start_distance = std::clamp(a_settings.fade_start_distance, 0.0f, a_settings.max_distance);
            a_settings.fade_power = std::clamp(a_settings.fade_power, 0.1f, 16.0f);
            a_settings.high_value_threshold = std::max(0.0f, a_settings.high_value_threshold);
            a_settings.book_filter_mode = std::clamp(a_settings.book_filter_mode, 0, 2);
        }
    }

    void load() noexcept
    {
        CSimpleIniA ini;
        ini.SetUnicode();

        auto const path = get_ini_path();
        SI_Error const rc = ini.LoadFile(path.string().c_str());
        if (rc < 0)
            logger::info("INI not found at {}, writing defaults", path.string());

        g_settings.enabled = ini.GetBoolValue("General", "Enabled", g_settings.enabled);
        g_settings.hotkey = static_cast<std::uint32_t>(ini.GetLongValue("General", "Hotkey", static_cast<long>(g_settings.hotkey)));
        g_settings.max_distance = static_cast<float>(ini.GetDoubleValue("General", "MaxDistance", g_settings.max_distance));
        g_settings.scan_interval_ms = static_cast<std::uint32_t>(ini.GetLongValue("General", "ScanIntervalMs", static_cast<long>(g_settings.scan_interval_ms)));

        g_settings.outline_color = parse_hex(ini.GetValue("Display", "OutlineColor", "00FF66"), 0x00FF66);
        g_settings.min_opacity = static_cast<float>(ini.GetDoubleValue("Display", "MinOpacity", g_settings.min_opacity));
        g_settings.outline_thickness = static_cast<float>(ini.GetDoubleValue("Display", "OutlineThickness", g_settings.outline_thickness));
        g_settings.fade_start_distance = static_cast<float>(ini.GetDoubleValue("Display", "FadeStartDistance", g_settings.fade_start_distance));
        g_settings.fade_power = static_cast<float>(ini.GetDoubleValue("Display", "FadePower", g_settings.fade_power));

        g_settings.hide_searched_enabled = ini.GetBoolValue("LootFilter", "HideSearchedEnabled", g_settings.hide_searched_enabled);
        g_settings.value_filter_enabled = ini.GetBoolValue("LootFilter", "ValueFilterEnabled", g_settings.value_filter_enabled);
        g_settings.value_quest_items = ini.GetBoolValue("LootFilter", "ValueQuestItems", g_settings.value_quest_items);
        g_settings.value_keys = ini.GetBoolValue("LootFilter", "ValueKeys", g_settings.value_keys);
        g_settings.value_enchanted = ini.GetBoolValue("LootFilter", "ValueEnchanted", g_settings.value_enchanted);
        g_settings.value_high_value = ini.GetBoolValue("LootFilter", "ValueHighValue", g_settings.value_high_value);
        g_settings.high_value_threshold = static_cast<float>(ini.GetDoubleValue("LootFilter", "HighValueThreshold", g_settings.high_value_threshold));
        g_settings.value_books = ini.GetBoolValue("LootFilter", "ValueBooks", g_settings.value_books);
        g_settings.book_filter_mode = static_cast<int>(ini.GetLongValue("LootFilter", "BookFilterMode", static_cast<long>(g_settings.book_filter_mode)));
        g_settings.value_consumables = ini.GetBoolValue("LootFilter", "ValueConsumables", g_settings.value_consumables);

        sanitize(g_settings);
        g_enabled.store(g_settings.enabled, std::memory_order_relaxed);

        // 写回，保证文件存在且写出的是规范化后的取值
        save();

        logger::info(
            "Config loaded: enabled={}, hotkey=0x{:02X}, max_distance={:.0f}, scanInterval={}ms, lootFilter={}",
            g_settings.enabled,
            g_settings.hotkey,
            g_settings.max_distance,
            g_settings.scan_interval_ms,
            g_settings.value_filter_enabled);
    }

    void save() noexcept
    {
        // 手写模板写出（SimpleIni 不支持写注释）。每个选项的注释置于其上一行
        // （行尾注释在 INI 里难以对齐排版），款式与 README_EN.md 的
        // "Configuration reference" INI 示例一致，两处必须同步修改。
        // 写出后清除 MCP 菜单改动标记。
        std::filesystem::path const path = get_ini_path();

        auto const section = [&](std::string_view a_name) {
            return fmt::format("[{}]\n", a_name);
        };
        auto const option = [](std::string_view a_comment, std::string_view a_kv) {
            return fmt::format("; {}\n{}\n", a_comment, a_kv);
        };

        std::string body;
        body += section("General");
        body += option("mod enabled on startup", fmt::format("Enabled={}", g_settings.enabled ? "true" : "false"));
        body += option("toggle key virtual-key code (0 = disabled, rebindable in the MCP menu)", fmt::format("Hotkey={}", g_settings.hotkey));
        body += option("search radius in game units (~17 m default)", fmt::format("MaxDistance={:.1f}", g_settings.max_distance));
        body += option("corpse scan interval in milliseconds", fmt::format("ScanIntervalMs={}", g_settings.scan_interval_ms));
        body += section("Display");
        body += option("outline color (RGB hex)", fmt::format("OutlineColor={:06X}", g_settings.outline_color));
        body += option("minimum opacity at max distance", fmt::format("MinOpacity={:.2f}", g_settings.min_opacity));
        body += option("outline thickness in pixels", fmt::format("OutlineThickness={:.1f}", g_settings.outline_thickness));
        body += option("distance where fading begins (fully opaque below)", fmt::format("FadeStartDistance={:.1f}", g_settings.fade_start_distance));
        body += option("fade curve exponent (higher = faster fade)", fmt::format("FadePower={:.1f}", g_settings.fade_power));
        body += section("LootFilter");
        body += option("stop outlining corpses the player has searched (activated) at least once, even if nothing was taken", fmt::format("HideSearchedEnabled={}", g_settings.hide_searched_enabled ? "true" : "false"));
        body += option("only outline corpses matching the categories below", fmt::format("ValueFilterEnabled={}", g_settings.value_filter_enabled ? "true" : "false"));
        body += option("quest items", fmt::format("ValueQuestItems={}", g_settings.value_quest_items ? "true" : "false"));
        body += option("keys", fmt::format("ValueKeys={}", g_settings.value_keys ? "true" : "false"));
        body += option("enchanted equipment", fmt::format("ValueEnchanted={}", g_settings.value_enchanted ? "true" : "false"));
        body += option("single item worth >= HighValueThreshold gold", fmt::format("ValueHighValue={}", g_settings.value_high_value ? "true" : "false"));
        body += option("high-value threshold (gold piles count by amount)", fmt::format("HighValueThreshold={:.1f}", g_settings.high_value_threshold));
        body += option("books", fmt::format("ValueBooks={}", g_settings.value_books ? "true" : "false"));
        body += option("0 = all books, 1 = spell & skill books, 2 = spell books only", fmt::format("BookFilterMode={}", g_settings.book_filter_mode));
        body += option("arrows, ingredients, potions, scrolls, soul gems", fmt::format("ValueConsumables={}", g_settings.value_consumables ? "true" : "false"));

        std::ofstream file(path, std::ios::binary);
        if (!file)
        {
            logger::warn("Failed to write INI at {}", path.string());
            return;
        }
        file.write(body.c_str(), static_cast<std::streamsize>(body.size()));

        if (!file)
        {
            logger::warn("Failed to write INI at {}", path.string());
            return;
        }
        g_dirty = false;
    }

    void mark_dirty() noexcept
    {
        g_dirty = true;
    }

    void save_if_dirty() noexcept
    {
        // 仅在 MCP 菜单改动过设置时落盘（免手动 "Save to INI"）；未改动则
        // 不重写文件，保留用户手改的 INI。由 SKSE kSaveGame 消息触发——
        // 玩家存档时引擎状态健康，且是设置固化的自然时机
        if (!g_dirty)
            return;

        save();
        logger::info("Config saved on game save (menu changes pending)");
    }

    void reset_defaults() noexcept
    {
        g_settings = Settings{};
        g_enabled.store(g_settings.enabled, std::memory_order_relaxed);
        save();
        logger::info("Config reset to defaults");
    }

    Settings const& get() noexcept
    {
        return g_settings;
    }

    Settings& get_mutable() noexcept
    {
        return g_settings;
    }

    bool is_enabled() noexcept
    {
        return g_enabled.load(std::memory_order_relaxed);
    }

    void set_enabled(bool a_enabled) noexcept
    {
        g_settings.enabled = a_enabled;
        g_enabled.store(a_enabled, std::memory_order_relaxed);
    }

    void save_enabled() noexcept
    {
        // 热键切换后落盘。直接 save()（带注释模板写全量）而非读改写：
        // 读改写会经由 SimpleIni 输出，抹掉 save() 写入的注释
        save();
    }
}
