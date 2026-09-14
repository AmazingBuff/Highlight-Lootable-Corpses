#include "ui_menu.h"
#include "config/config.h"
#include "input/pulse_highlight.h"
#include "search/corpse_finder.h"
#include "render/render_util.h"

#pragma warning(push)
#pragma warning(disable: 4996 5054 4099 4267 4244 4061 4062)
#include <SKSEMCP/utils.hpp>
#pragma warning(pop)

PLUGIN_NAMESPACE_BEGIN

namespace
{
    std::string hotkey_name(std::uint32_t a_vk)
    {
        if (a_vk == 0)
            return "None";

        static constexpr std::string_view s_key_names[] = {
            "Backspace"sv, "Tab"sv, ""sv, ""sv, ""sv, "Enter"sv, ""sv, ""sv,   // 0x08-0x0F
            "Shift"sv, "Ctrl"sv, "Alt"sv, "Pause"sv, "Caps"sv, ""sv, ""sv, ""sv, ""sv, ""sv, ""sv, "Esc"sv, ""sv, ""sv, ""sv, ""sv,   // 0x10-0x1F
            "Space"sv, "PgUp"sv, "PgDn"sv, "End"sv, "Home"sv, "Left"sv, "Up"sv, "Right"sv, "Down"sv, ""sv, ""sv, ""sv, ""sv, "Ins"sv, "Del"sv,   // 0x20-0x2E
        };
        if (a_vk >= 0x08 && a_vk <= 0x2E)
        {
            const std::string_view name = s_key_names[a_vk - 0x08];
            if (!name.empty())
                return name.data();
        }
        if (a_vk >= 0x30 && a_vk <= 0x39)
            return {1, static_cast<char>(a_vk)};                        // 0-9
        if (a_vk >= 0x41 && a_vk <= 0x5A)
            return {1, static_cast<char>(a_vk)};                        // A-Z
        if (a_vk >= 0x60 && a_vk <= 0x69)
            return fmt::format("Num {}", a_vk - 0x60);                             // 小键盘 0-9
        if (a_vk >= 0x70 && a_vk <= 0x87)
            return fmt::format("F{}", a_vk - 0x6F);                                // F1-F24

        switch (a_vk)
        {
        case 0x01: return "LMB";
        case 0x02: return "RMB";
        case 0x04: return "MMB";
        case 0x05: return "Mouse 4";
        case 0x06: return "Mouse 5";
        default:
            logger::warn("Unsupported hotkey {}!", a_vk);
        }
        return fmt::format("0x{:02X}", a_vk);
    }

    // MCP 菜单回调：游戏主线程执行（框架在 imgui 帧内调用），
    // 直接读写 Config 设置；修改即时生效，渲染线程无锁读取（与 set_enabled 同模式）。
    void render_settings()
    {
        Config& cfg = Setting::get_config();

        ImGuiMCP::Checkbox("Enabled", &cfg.enabled);

        static bool s_rebinding = false;
        std::string const label = s_rebinding ? std::string("Press any key...") : fmt::format("Hotkey: {}", hotkey_name(cfg.hotkey));
        if (ImGuiMCP::Button(label.c_str()))
            s_rebinding = !s_rebinding;

        static constexpr Config::HotkeyMode s_hotkey_modes[] = {
            Config::HotkeyMode::e_constant,
            Config::HotkeyMode::e_pulse,
        };
        static constexpr char const* s_hotkey_mode_names[] = { "Constant", "Pulse" };
        std::size_t hk_index = static_cast<std::size_t>(cfg.hotkey_mode);
        if (ImGuiMCP::Button(fmt::format("Hotkey Mode: {}", s_hotkey_mode_names[hk_index]).c_str()))
        {
            hk_index = (hk_index + 1) % std::size(s_hotkey_modes);
            Config::HotkeyMode const previous = cfg.hotkey_mode;
            cfg.hotkey_mode = s_hotkey_modes[hk_index];
            if (previous == Config::HotkeyMode::e_constant && cfg.hotkey_mode == Config::HotkeyMode::e_pulse && cfg.enabled)
                PulseHighlight::trigger(cfg.pulse_duration_ms);
            else if (previous == Config::HotkeyMode::e_pulse && cfg.hotkey_mode == Config::HotkeyMode::e_constant)
                PulseHighlight::reset();
        }

        if (cfg.hotkey_mode == Config::HotkeyMode::e_pulse)
            ImGuiMCP::SliderInt("Pulse Duration (ms)", &cfg.pulse_duration_ms, Setting::Min_Pulse_Duration_Ms, Setting::Max_Pulse_Duration_Ms);

        ImGuiMCP::SliderInt("Scan Interval (ms)", &cfg.scan_interval_ms, Setting::Min_Scan_Interval, Setting::Max_Scan_Interval);

        ImGuiMCP::Separator();

        static constexpr Config::DisplayMode s_display_modes[] = {
            Config::DisplayMode::e_silhouette,
            Config::DisplayMode::e_outline,
            Config::DisplayMode::e_icon,
        };
        static constexpr char const* s_display_mode_names[] = { "Silhouette", "Outline", "Icon" };
        std::size_t mode_index = static_cast<std::size_t>(cfg.display_mode);
        if (ImGuiMCP::Button(fmt::format("Display Mode: {}", s_display_mode_names[mode_index]).c_str()))
        {
            mode_index = (mode_index + 1) % std::size(s_display_modes);
            cfg.display_mode = s_display_modes[mode_index];
        }
        if (cfg.display_mode == Config::DisplayMode::e_outline)
            ImGuiMCP::SliderInt("Outline Thickness", &cfg.outline_thickness, Setting::Min_Outline_Thickness, Setting::Max_Outline_Thickness);
        else if (cfg.display_mode == Config::DisplayMode::e_icon)
            ImGuiMCP::SliderInt("Icon Radius", &cfg.icon_radius, Setting::Min_Icon_Radius, Setting::Max_Icon_Radius);

        Color rgb;
        rgb.decode(cfg.outline_color);
        if (ImGuiMCP::ColorEdit4("Outline Color", reinterpret_cast<float*>(&rgb)))
            cfg.outline_color = rgb.encode();

        ImGuiMCP::SliderFloat("Min Opacity", &cfg.min_opacity, 0.0f, 1.0f, "%.2f");
        ImGuiMCP::SliderFloat("Max Search Distance", &cfg.max_distance, Setting::Min_Max_Distance, Setting::Max_Max_Distance, "%.0f");
        ImGuiMCP::SliderFloat("Fade Start Distance", &cfg.fade_start_distance, 0.0f, cfg.max_distance, "%.0f");
        ImGuiMCP::SliderFloat("Fade Power", &cfg.fade_power, Setting::Min_Fade_Power, Setting::Max_Fade_Power, "%.1f");

        ImGuiMCP::Separator();

        ImGuiMCP::Checkbox("Hide Searched Corpses", &cfg.hide_searched_enabled);

        ImGuiMCP::Checkbox("Enable Value Filter", &cfg.value_filter_enabled);
        ImGuiMCP::BeginDisabled(!cfg.value_filter_enabled);
        ImGuiMCP::Checkbox("Quest Items", &cfg.value_quest_items);
        ImGuiMCP::Checkbox("Keys", &cfg.value_keys);
        ImGuiMCP::Checkbox("Enchanted Gear", &cfg.value_enchanted);
        ImGuiMCP::Checkbox("High-Value Items", &cfg.value_high_value);
        ImGuiMCP::SliderInt("High Value Threshold", &cfg.high_value_threshold, Setting::Min_High_Value_Threshold, Setting::Max_High_Value_Threshold);

        static constexpr char const* s_book_modes[] = { "Spell Books", "Skill Books", "Unread Books" };
        bool book_mode[] = {
            static_cast<bool>(cfg.book_filter_mode & Config::BookType::e_spell),
            static_cast<bool>(cfg.book_filter_mode & Config::BookType::e_skill),
            static_cast<bool>(cfg.book_filter_mode & Config::BookType::e_not_read)
        };

        ImGuiMCP::Checkbox(s_book_modes[0], &book_mode[0]);
        ImGuiMCP::SameLine();
        ImGuiMCP::Checkbox(s_book_modes[1], &book_mode[1]);
        ImGuiMCP::SameLine();
        ImGuiMCP::Checkbox(s_book_modes[2], &book_mode[2]);

        cfg.book_filter_mode = Config::BookType::e_none;
        if (book_mode[0])
            cfg.book_filter_mode |= Config::BookType::e_spell;
        if (book_mode[1])
            cfg.book_filter_mode |= Config::BookType::e_skill;
        if (book_mode[2])
            cfg.book_filter_mode |= Config::BookType::e_not_read;


        ImGuiMCP::Checkbox("Consumables", &cfg.value_consumables);
        ImGuiMCP::EndDisabled();

        ImGuiMCP::Separator();

        std::vector<CorpseScan::CorpseInfo> const corpses = CorpseScan::snapshot();
        float nearest = 0.0f;
        for (auto const& corpse : corpses)
            nearest = nearest == 0.0f ? corpse.distance : std::min(nearest, corpse.distance);

        ImGuiMCP::Text("Corpses: %d | Nearest: %.0f units", static_cast<int>(corpses.size()), nearest);

        if (ImGuiMCP::Button("Save"))
            Setting::save();
    }
}


bool Menu::is_menu_open()
{
    return SKSEMenuFramework::IsInstalled() && SKSEMenuFramework::IsAnyBlockingWindowOpened();
}

void Menu::register_menu()
{
    static bool s_registered = false;
    if (s_registered)
        return;

    if (!SKSEMenuFramework::IsInstalled())
    {
        logger::warn("SKSE Menu Framework (SKSEMenuFramework.dll) not installed, in-game settings menu disabled");
        return;
    }

    SKSEMenuFramework::SetSection("Highlight Lootable Corpses");
    SKSEMenuFramework::AddSectionItem("Settings", render_settings);
    s_registered = true;

    logger::info("Registered Highlight Lootable Corpses settings page (SKSE Menu Framework v{:.2f})", SKSEMenuFramework::GetMenuFrameworkVersion());
}

PLUGIN_NAMESPACE_END