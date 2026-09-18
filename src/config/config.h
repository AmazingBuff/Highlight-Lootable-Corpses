#pragma once

PLUGIN_NAMESPACE_BEGIN

struct Config
{
    bool enabled;
    uint32_t hotkey;
    enum class HotkeyMode : uint8_t
    {
        e_constant = 0,
        e_pulse    = 1
    };

    HotkeyMode hotkey_mode;
    int pulse_duration_ms;
    int scan_interval_ms;

    enum class DisplayMode : uint8_t
    {
        e_silhouette = 0,
        e_outline    = 1,
        e_icon       = 2
    };

    DisplayMode display_mode;
    int outline_thickness;
    int icon_radius;
    uint32_t outline_color;
    float min_opacity;
    float max_distance;
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
    Setting(Setting const&) = delete;
    Setting(Setting const&&) = delete;
    Setting operator=(Setting&) = delete;
    Setting operator=(Setting&&) = delete;

    static Setting& instance();

    Config& get_config() noexcept;

    void load() noexcept;
    void save() noexcept;

public:
    constexpr static int Min_Outline_Thickness = 1;
    constexpr static int Max_Outline_Thickness = 5;

    constexpr static int Min_Icon_Radius = 5;
    constexpr static int Max_Icon_Radius = 20;

    constexpr static float Min_Max_Distance = 500.f;
    constexpr static float Max_Max_Distance = 5000.f;

    constexpr static int Min_Scan_Interval = 100;
    constexpr static int Max_Scan_Interval = 1000;

    constexpr static float Min_Fade_Power = 0.1f;
    constexpr static float Max_Fade_Power = 4.f;

    constexpr static int Min_High_Value_Threshold = 0;
    constexpr static int Max_High_Value_Threshold = 500;

    constexpr static int Min_Pulse_Duration_Ms = 500;
    constexpr static int Max_Pulse_Duration_Ms = 30000;
private:
    Setting();
    ~Setting();
private:
    Config m_config;
};

PLUGIN_NAMESPACE_END