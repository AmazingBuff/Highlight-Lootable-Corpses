#include "config.h"

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
        a_settings.hotkey_mode = a_settings.hotkey_mode > Config::HotkeyMode::e_pulse
                                     ? Config::HotkeyMode::e_constant
                                     : a_settings.hotkey_mode;
        a_settings.pulse_duration_ms = std::clamp(a_settings.pulse_duration_ms, Setting::Min_Pulse_Duration_Ms, Setting::Max_Pulse_Duration_Ms);
        a_settings.scan_interval_ms = std::clamp(a_settings.scan_interval_ms, Setting::Min_Scan_Interval, Setting::Max_Scan_Interval);
        a_settings.display_mode = a_settings.display_mode > Config::DisplayMode::e_icon
                                      ? Config::DisplayMode::e_outline
                                      : a_settings.display_mode;
        a_settings.outline_thickness = std::clamp(a_settings.outline_thickness, Setting::Min_Outline_Thickness, Setting::Max_Outline_Thickness);
        a_settings.icon_radius = std::clamp(a_settings.icon_radius, Setting::Min_Icon_Radius, Setting::Max_Icon_Radius);
        a_settings.min_opacity = std::clamp(a_settings.min_opacity, 0.0f, 1.0f);
        a_settings.max_distance = std::clamp(a_settings.max_distance, Setting::Min_Max_Distance, Setting::Max_Max_Distance);
        a_settings.fade_start_distance = std::clamp(a_settings.fade_start_distance, 0.0f, a_settings.max_distance);
        a_settings.fade_power = std::clamp(a_settings.fade_power, Setting::Min_Fade_Power, Setting::Max_Fade_Power);
        a_settings.high_value_threshold = std::clamp(a_settings.high_value_threshold, Setting::Min_High_Value_Threshold, Setting::Max_High_Value_Threshold);
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
    g_config.hotkey_mode = static_cast<Config::HotkeyMode>(ini.GetLongValue("General", "HotkeyMode"));
    g_config.pulse_duration_ms = ini.GetLongValue("General", "PulseDurationMs");
    g_config.scan_interval_ms = ini.GetLongValue("General", "ScanIntervalMs");

    g_config.display_mode = static_cast<Config::DisplayMode>(ini.GetLongValue("Display", "DisplayMode"));
    g_config.outline_thickness = ini.GetLongValue("Display", "OutlineThickness");
    g_config.icon_radius = ini.GetLongValue("Display", "IconRadius");
    g_config.outline_color = parse_hex(ini.GetValue("Display", "OutlineColor"), 0x00FF66);
    g_config.min_opacity = static_cast<float>(ini.GetDoubleValue("Display", "MinOpacity"));
    g_config.max_distance = static_cast<float>(ini.GetDoubleValue("Display", "MaxDistance"));
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
    static auto const section = [](std::string_view a_name) {
        return fmt::format("[{}]\n", a_name);
    };
    static auto const option = [](std::string_view a_comment, std::string_view a_kv) {
        return fmt::format("; {}\n{}\n", a_comment, a_kv);
    };

    std::string body;
    body += section("General");
    body += option("mod enabled on startup", fmt::format("Enabled={}", g_config.enabled ? "true" : "false"));
    body += option("toggle key virtual-key code (0 = disabled, rebindable in the MCP menu)", fmt::format("Hotkey={}", g_config.hotkey));
    body += option("hotkey behavior: constant (0, toggle on/off) | pulse (1, highlight unsearched corpses then fade out)", fmt::format("HotkeyMode={}", static_cast<int>(g_config.hotkey_mode)));
    body += option("pulse mode: highlight lifetime in milliseconds before fully fading out", fmt::format("PulseDurationMs={}", g_config.pulse_duration_ms));
    body += option("corpse scan interval in milliseconds", fmt::format("ScanIntervalMs={}", g_config.scan_interval_ms));
    body += section("Display");
    body += option("corpse display style: silhouette (filled mask, 0) | outline (band around the mask, 1) | icon (distance-scaled arrows above corpses; nearby crowded targets share a double arrow, 2)",fmt::format("DisplayMode={}", static_cast<int>(g_config.display_mode)));
    body += option("outline thickness in pixels", fmt::format("OutlineThickness={}", g_config.outline_thickness));
    body += option("icon base half-width in pixels; distance scaling 0.75-1.25, groups 1.2x (maximum 1.5x)", fmt::format("IconRadius={}", g_config.icon_radius));
    body += option("outline color (ARGB hex)", fmt::format("OutlineColor={:06X}", g_config.outline_color));
    body += option("minimum opacity at max distance", fmt::format("MinOpacity={:.2f}", g_config.min_opacity));
    body += option("search radius in game units (~17 m default)", fmt::format("MaxDistance={:.1f}", g_config.max_distance));
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
    body += option("bit flag, 1 for spell, 2 for skill, 4 for unread, 7 for all", fmt::format("BookFilterMode={:01X}", static_cast<int>(g_config.book_filter_mode.underlying())));
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

    file.close();
}

Config& Setting::get_config() noexcept
{
    return g_config;
}
PLUGIN_NAMESPACE_END
