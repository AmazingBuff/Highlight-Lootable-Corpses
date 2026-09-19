#include "config.h"

#include <SimpleIni.h>

PLUGIN_NAMESPACE_BEGIN

static constexpr std::string_view Config_Instruction = R"(
[General]
; mod enabled on startup
Enabled={}
; toggle key virtual-key code (0 = disabled, rebindable in the MCP menu)
Hotkey={}
; hotkey behavior: constant (0, toggle on/off) | pulse (1, highlight unsearched corpses then fade out)
HotkeyMode={}
; pulse mode: highlight lifetime in milliseconds before fully fading out
PulseDurationMs={}
; corpse scan interval in milliseconds
ScanIntervalMs={}
[Display]
; corpse display style: silhouette (filled mask, 0) | outline (bright core plus outward glow, 1) | icon (distance-scaled arrows above corpses; nearby crowded targets share a double arrow, 2)
; usually, icon mode has best performance, then silhouette mode, outline is the worst
DisplayMode={}
; outline glow size (1-5; larger values widen the bright rim and outer halo)
OutlineThickness={}
; icon base half-width in pixels; distance scaling 0.75-1.25, groups 1.2x (maximum 1.5x)
IconRadius={}
; outline color (ARGB hex)
OutlineColor={:06X}
; minimum opacity at max distance
MinOpacity={:.2f}
; search radius in game units (~17 m default)
MaxDistance={:.1f}
; distance where fading begins (fully opaque below)
FadeStartDistance={:.1f}
; fade curve exponent (higher = faster fade)
FadePower={:.1f}
[LootFilter]
; stop outlining corpses the player has searched (activated) at least once, even if nothing was taken
HideSearchedEnabled={}
; only outline corpses matching the categories below
ValueFilterEnabled={}
; quest items
ValueQuestItems={}
; keys
ValueKeys={}
; enchanted equipment
ValueEnchanted={}
; single item worth >= HighValueThreshold gold
ValueHighValue={}
; high-value threshold (gold piles count by amount)
HighValueThreshold={}
; bit flag, 1 for spell, 2 for skill, 4 for unread, 7 for all
BookFilterMode={:01X}
; arrows, ingredients, potions, scrolls, soul gems
ValueConsumables={}
)";

namespace
{
    uint32_t parse_hex(char const* a_value, uint32_t a_default) noexcept
    {
        if (!a_value || !*a_value)
            return a_default;

        char* end = nullptr;
        uint32_t const value = std::strtoul(a_value, &end, 16);
        return end == a_value ? a_default : value;
    }

    void sanitize(Config& a_settings) noexcept
    {
        a_settings.hotkey = a_settings.hotkey > 0xFEu ? 0u : a_settings.hotkey;  // 0 = not bound
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

    std::string const& get_config_path() noexcept
    {
        static std::string const s_config_path = "Data/SKSE/Plugins/" + std::string(Plugin::Plugin_Name) + ".ini";
        return s_config_path;
    }
}

Setting& Setting::instance()
{
    static Setting s_instance;
    return s_instance;
}

void Setting::load() noexcept
{
    CSimpleIniA ini;
    ini.SetUnicode();

    std::string const& path = get_config_path();
    if (SI_Error const rc = ini.LoadFile(path.c_str()); rc < 0)
        logger::info("INI not found at {}, writing defaults", path);

    m_config.enabled = ini.GetBoolValue("General", "Enabled");
    m_config.hotkey = static_cast<uint32_t>(ini.GetLongValue("General", "Hotkey"));
    m_config.hotkey_mode = static_cast<Config::HotkeyMode>(ini.GetLongValue("General", "HotkeyMode"));
    m_config.pulse_duration_ms = ini.GetLongValue("General", "PulseDurationMs");
    m_config.scan_interval_ms = ini.GetLongValue("General", "ScanIntervalMs");

    m_config.display_mode = static_cast<Config::DisplayMode>(ini.GetLongValue("Display", "DisplayMode"));
    m_config.outline_thickness = ini.GetLongValue("Display", "OutlineThickness");
    m_config.icon_radius = ini.GetLongValue("Display", "IconRadius");
    m_config.outline_color = parse_hex(ini.GetValue("Display", "OutlineColor"), 0x00FF66);
    m_config.min_opacity = static_cast<float>(ini.GetDoubleValue("Display", "MinOpacity"));
    m_config.max_distance = static_cast<float>(ini.GetDoubleValue("Display", "MaxDistance"));
    m_config.fade_start_distance = static_cast<float>(ini.GetDoubleValue("Display", "FadeStartDistance"));
    m_config.fade_power = static_cast<float>(ini.GetDoubleValue("Display", "FadePower"));

    m_config.hide_searched_enabled = ini.GetBoolValue("LootFilter", "HideSearchedEnabled");
    m_config.value_filter_enabled = ini.GetBoolValue("LootFilter", "ValueFilterEnabled");
    m_config.value_quest_items = ini.GetBoolValue("LootFilter", "ValueQuestItems");
    m_config.value_keys = ini.GetBoolValue("LootFilter", "ValueKeys");
    m_config.value_enchanted = ini.GetBoolValue("LootFilter", "ValueEnchanted");
    m_config.value_high_value = ini.GetBoolValue("LootFilter", "ValueHighValue");
    m_config.high_value_threshold = ini.GetLongValue("LootFilter", "HighValueThreshold");
    m_config.book_filter_mode = static_cast<Config::BookType>(ini.GetLongValue("LootFilter", "BookFilterMode"));
    m_config.value_consumables = ini.GetBoolValue("LootFilter", "ValueConsumables");

    sanitize(m_config);

    logger::info("Config loaded!");
}

void Setting::save() noexcept
{
    const std::string body = fmt::format(Config_Instruction,
        m_config.enabled ? "true" : "false",
        m_config.hotkey,
        static_cast<int>(m_config.hotkey_mode),
        m_config.pulse_duration_ms,
        m_config.scan_interval_ms,
        static_cast<int>(m_config.display_mode),
        m_config.outline_thickness,
        m_config.icon_radius,
        m_config.outline_color,
        m_config.min_opacity,
        m_config.max_distance,
        m_config.fade_start_distance,
        m_config.fade_power,
        m_config.hide_searched_enabled ? "true" : "false",
        m_config.value_filter_enabled ? "true" : "false",
        m_config.value_quest_items ? "true" : "false",
        m_config.value_keys ? "true" : "false",
        m_config.value_enchanted ? "true" : "false",
        m_config.value_high_value ? "true" : "false",
        m_config.high_value_threshold,
        static_cast<int>(m_config.book_filter_mode.underlying()),
        m_config.value_consumables ? "true" : "false"
        );

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
    return m_config;
}

Setting::Setting() : m_config{} {}

Setting::~Setting() = default;

PLUGIN_NAMESPACE_END
