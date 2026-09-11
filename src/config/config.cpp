#include "config.h"

#include <fstream>
#include <SimpleIni.h>

PLUGIN_NAMESPACE_BEGIN

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

    void sanitize(Config& a_settings) noexcept
    {
        a_settings.hotkey = a_settings.hotkey > 0xFEu ? 0u : a_settings.hotkey;  // 0 = 不绑定
        a_settings.max_distance = std::clamp(a_settings.max_distance, 100.0f, 100000.0f);
        a_settings.scan_interval_ms = std::clamp(a_settings.scan_interval_ms, 50, 60000);
        a_settings.outline_color &= 0x00FFFFFFu;
        a_settings.min_opacity = std::clamp(a_settings.min_opacity, 0.0f, 1.0f);
        a_settings.outline_thickness = std::clamp(a_settings.outline_thickness, 1.0f, 16.0f);
        a_settings.fade_start_distance = std::clamp(a_settings.fade_start_distance, 0.0f, a_settings.max_distance);
        a_settings.fade_power = std::clamp(a_settings.fade_power, 0.1f, 16.0f);
        a_settings.high_value_threshold = std::max(0, a_settings.high_value_threshold);
        a_settings.book_filter_mode = static_cast<Config::BookType>(
            std::clamp(a_settings.book_filter_mode.underlying(),
            static_cast<std::underlying_type_t<Config::BookType>>(Config::BookType::e_none),
            static_cast<std::underlying_type_t<Config::BookType>>(Config::BookType::e_all)));
    }

    const std::string& get_config_path() noexcept
    {
        static const std::string s_config_path = "Data/SKSE/Plugins/" + std::string(Plugin::Plugin_Name) + ".ini";
        return s_config_path;
    }

    Config g_config;
}

void Setting::load() noexcept
{
    CSimpleIniA ini;
    ini.SetUnicode();

    std::string const& path = get_config_path();
    if (SI_Error const rc = ini.LoadFile(path.c_str()); rc < 0)
        logger::info("INI not found at {}, writing defaults", path);

    g_config.enabled = ini.GetBoolValue("General", "Enabled");
    g_config.hotkey = static_cast<std::uint32_t>(ini.GetLongValue("General", "Hotkey"));
    g_config.max_distance = static_cast<float>(ini.GetDoubleValue("General", "MaxDistance"));
    g_config.scan_interval_ms = ini.GetLongValue("General", "ScanIntervalMs");

    g_config.outline_color = parse_hex(ini.GetValue("Display", "OutlineColor"), 0x00FF66);
    g_config.min_opacity = static_cast<float>(ini.GetDoubleValue("Display", "MinOpacity"));
    g_config.outline_thickness = static_cast<float>(ini.GetDoubleValue("Display", "OutlineThickness"));
    g_config.fade_start_distance = static_cast<float>(ini.GetDoubleValue("Display", "FadeStartDistance"));
    g_config.fade_power = static_cast<float>(ini.GetDoubleValue("Display", "FadePower"));

    g_config.hide_searched_enabled = ini.GetBoolValue("LootFilter", "HideSearchedEnabled");
    g_config.value_filter_enabled = ini.GetBoolValue("LootFilter", "ValueFilterEnabled");
    g_config.value_quest_items = ini.GetBoolValue("LootFilter", "ValueQuestItems");
    g_config.value_keys = ini.GetBoolValue("LootFilter", "ValueKeys");
    g_config.value_enchanted = ini.GetBoolValue("LootFilter", "ValueEnchanted");
    g_config.value_high_value = ini.GetBoolValue("LootFilter", "ValueHighValue");
    g_config.high_value_threshold = ini.GetLongValue("LootFilter", "HighValueThreshold");
    g_config.book_filter_mode = static_cast<Config::BookType>(ini.GetLongValue("LootFilter", "BookFilterMode"));
    g_config.value_consumables = ini.GetBoolValue("LootFilter", "ValueConsumables");

    sanitize(g_config);

    logger::info("Config loaded!");
}

void Setting::save() noexcept
{
    auto const section = [&](std::string_view a_name) {
        return fmt::format("[{}]\n", a_name);
    };
    auto const option = [](std::string_view a_comment, std::string_view a_kv) {
        return fmt::format("; {}\n{}\n", a_comment, a_kv);
    };

    std::string body;
    body += section("General");
    body += option("mod enabled on startup", fmt::format("Enabled={}", g_config.enabled ? "true" : "false"));
    body += option("toggle key virtual-key code (0 = disabled, rebindable in the MCP menu)", fmt::format("Hotkey={}", g_config.hotkey));
    body += option("search radius in game units (~17 m default)", fmt::format("MaxDistance={:.1f}", g_config.max_distance));
    body += option("corpse scan interval in milliseconds", fmt::format("ScanIntervalMs={}", g_config.scan_interval_ms));
    body += section("Display");
    body += option("outline color (RGB hex)", fmt::format("OutlineColor={:06X}", g_config.outline_color));
    body += option("minimum opacity at max distance", fmt::format("MinOpacity={:.2f}", g_config.min_opacity));
    body += option("outline thickness in pixels", fmt::format("OutlineThickness={:.1f}", g_config.outline_thickness));
    body += option("distance where fading begins (fully opaque below)", fmt::format("FadeStartDistance={:.1f}", g_config.fade_start_distance));
    body += option("fade curve exponent (higher = faster fade)", fmt::format("FadePower={:.1f}", g_config.fade_power));
    body += section("LootFilter");
    body += option("stop outlining corpses the player has searched (activated) at least once, even if nothing was taken", fmt::format("HideSearchedEnabled={}", g_config.hide_searched_enabled ? "true" : "false"));
    body += option("only outline corpses matching the categories below", fmt::format("ValueFilterEnabled={}", g_config.value_filter_enabled ? "true" : "false"));
    body += option("quest items", fmt::format("ValueQuestItems={}", g_config.value_quest_items ? "true" : "false"));
    body += option("keys", fmt::format("ValueKeys={}", g_config.value_keys ? "true" : "false"));
    body += option("enchanted equipment", fmt::format("ValueEnchanted={}", g_config.value_enchanted ? "true" : "false"));
    body += option("single item worth >= HighValueThreshold gold", fmt::format("ValueHighValue={}", g_config.value_high_value ? "true" : "false"));
    body += option("high-value threshold (gold piles count by amount)", fmt::format("HighValueThreshold={}", g_config.high_value_threshold));
    body += option("bit flag, 0x1 for spell, 0x2 for skill, 0x4 for unread, 0x7 for all", fmt::format("BookFilterMode={:06X}", static_cast<int>(g_config.book_filter_mode.underlying())));
    body += option("arrows, ingredients, potions, scrolls, soul gems", fmt::format("ValueConsumables={}", g_config.value_consumables ? "true" : "false"));

    std::string const& path = get_config_path();
    std::ofstream file(path, std::ios::binary);
    if (!file)
    {
        logger::warn("Failed to write INI at {}", path);
        return;
    }
    file.write(body.c_str(), static_cast<std::streamsize>(body.size()));

    if (!file)
        logger::warn("Failed to write INI at {}", path);
}

Config& Setting::get_config() noexcept
{
    return g_config;
}
PLUGIN_NAMESPACE_END
