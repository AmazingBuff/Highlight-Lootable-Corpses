#pragma once

PLUGIN_NAMESPACE_BEGIN

struct Config
{
    bool enabled;
    uint32_t hotkey;
    float max_distance;
    int scan_interval_ms;

    enum class DisplayMode : std::uint8_t
    {
        e_silhouette = 0,
        e_outline    = 1,
        e_icon       = 2
    };

    DisplayMode display_mode;

    enum class HotkeyMode : std::uint8_t
    {
        e_constant = 0,  // 常亮：热键切换 enabled（既有行为）
        e_pulse    = 1   // 消退：热键触发一次脉冲高亮，highlight 后渐渐变淡直至消失
    };

    HotkeyMode hotkey_mode;
    std::uint32_t pulse_duration_ms;

    uint32_t outline_color;
    float min_opacity;
    float outline_thickness;

    float fade_start_distance;
    float fade_power;

    bool hide_searched_enabled;

    bool value_filter_enabled;
    bool value_quest_items;
    bool value_keys;
    bool value_enchanted;
    bool value_high_value;
    int high_value_threshold;

    enum class BookType : uint8_t
    {
        e_none      = 0,
        e_spell     = 1 << 0,
        e_skill     = 1 << 1,
        e_not_read  = 1 << 2,

        e_all       = 0xFF
    };

    RE::stl::enumeration<BookType> book_filter_mode;
    bool value_consumables;
};

class Setting
{
public:
    Setting() = delete;
    ~Setting() = delete;
    Setting(Setting const&) = delete;
    Setting(Setting const&&) = delete;
    Setting operator=(Setting&) = delete;
    Setting operator=(Setting&&) = delete;

    static Config& get_config() noexcept;

    static void load() noexcept;
    static void save() noexcept;

    constexpr static float Min_Max_Distance = 500.f;
    constexpr static float Max_Max_Distance = 5000.f;

    constexpr static int Min_Scan_Interval = 100;
    constexpr static int Max_Scan_Interval = 1000;

    constexpr static float Min_Outline_Thickness = 1.f;
    constexpr static float Max_Outline_Thickness = 3.f;

    constexpr static float Min_Fade_Power = 0.1f;
    constexpr static float Max_Fade_Power = 4.f;

    constexpr static int Min_High_Value_Threshold = 0;
    constexpr static int Max_High_Value_Threshold = 500;

    constexpr static std::uint32_t Min_Pulse_Duration_Ms = 500;
    constexpr static std::uint32_t Max_Pulse_Duration_Ms = 30000;
};

PLUGIN_NAMESPACE_END