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

    DisplayMode display_mode = DisplayMode::e_outline;  // 尸体显示样式（默认 outline）

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
};

PLUGIN_NAMESPACE_END