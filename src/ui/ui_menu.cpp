#include "ui_menu.h"
#include "config/config.h"
#include "input/pulse_highlight.h"
#include "search/corpse_finder.h"

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

        // 总开关：必须经 set_enabled 同步 g_enabled（渲染线程读它），直接改字段无效
        ImGuiMCP::Checkbox("Enabled", &cfg.enabled);

        static bool s_rebinding = false;
        std::string const label = s_rebinding ? std::string("Press any key...") : fmt::format("Hotkey: {}", hotkey_name(cfg.hotkey));
        if (ImGuiMCP::Button(label.c_str()))
            s_rebinding = !s_rebinding;

        // 显示模式三选一（契约 v13）：点击循环到下一模式，修改即时生效
        static constexpr Config::DisplayMode s_display_modes[] = {
            Config::DisplayMode::e_silhouette,
            Config::DisplayMode::e_outline,
            Config::DisplayMode::e_icon,
        };
        static constexpr char const* s_display_mode_names[] = { "Silhouette", "Outline", "Icon" };
        std::size_t mode_index = std::min<std::size_t>(static_cast<std::size_t>(cfg.display_mode), std::size(s_display_modes) - 1);
        if (ImGuiMCP::Button(fmt::format("Display Mode: {}", s_display_mode_names[mode_index]).c_str()))
        {
            mode_index = (mode_index + 1) % std::size(s_display_modes);
            cfg.display_mode = s_display_modes[mode_index];
        }

        // 热键模式二选一：constant=切换开关，pulse=触发一次渐隐高亮。即时生效；
        // enable 在切换中保持不变——pulse 模式下 enable 是脉冲前提（true 才能触发
        // 消退），常亮在途转 pulse 时以一次脉冲渐渐淡出。
        static constexpr Config::HotkeyMode s_hotkey_modes[] = {
            Config::HotkeyMode::e_constant,
            Config::HotkeyMode::e_pulse,
        };
        static constexpr char const* s_hotkey_mode_names[] = { "Constant", "Pulse" };
        std::size_t hk_index = std::min<std::size_t>(static_cast<std::size_t>(cfg.hotkey_mode), std::size(s_hotkey_modes) - 1);
        if (ImGuiMCP::Button(fmt::format("Hotkey Mode: {}", s_hotkey_mode_names[hk_index]).c_str()))
        {
            hk_index = (hk_index + 1) % std::size(s_hotkey_modes);
            Config::HotkeyMode const previous = cfg.hotkey_mode;
            cfg.hotkey_mode = s_hotkey_modes[hk_index];
            if (previous == Config::HotkeyMode::e_constant && cfg.hotkey_mode == Config::HotkeyMode::e_pulse && cfg.enabled)
                PulseHighlight::trigger(cfg.pulse_duration_ms);  // 常亮转脉冲：从满 alpha 开始渐隐
            else if (previous == Config::HotkeyMode::e_pulse && cfg.hotkey_mode == Config::HotkeyMode::e_constant)
                PulseHighlight::reset();
        }

        // 脉冲时长（仅 pulse 模式有意义）：修改即时生效，但只影响下一次脉冲
        //（在途脉冲已按触发时刻的时间戳走完自身曲线）。
        if (cfg.hotkey_mode == Config::HotkeyMode::e_pulse)
            ImGuiMCP::SliderInt("Pulse Duration (ms)", reinterpret_cast<int*>(&cfg.pulse_duration_ms), static_cast<int>(Setting::Min_Pulse_Duration_Ms), static_cast<int>(Setting::Max_Pulse_Duration_Ms));

        ImGuiMCP::SliderFloat("Max Search Distance", &cfg.max_distance, Setting::Min_Max_Distance, Setting::Max_Max_Distance, "%.0f");
        ImGuiMCP::SliderInt("Scan Interval (ms)", &cfg.scan_interval_ms, Setting::Min_Scan_Interval, Setting::Max_Scan_Interval);

        float color[3] = {
            static_cast<float>((cfg.outline_color >> 16) & 0xFF) / 255.0f,
            static_cast<float>((cfg.outline_color >> 8) & 0xFF) / 255.0f,
            static_cast<float>(cfg.outline_color & 0xFF) / 255.0f,
        };
        if (ImGuiMCP::ColorEdit3("Outline Color", color))
        {
            cfg.outline_color =
                (static_cast<std::uint32_t>(color[0] * 255.0f) << 16) |
                (static_cast<std::uint32_t>(color[1] * 255.0f) << 8) |
                static_cast<std::uint32_t>(color[2] * 255.0f);
        }

        ImGuiMCP::SliderFloat("Min Opacity", &cfg.min_opacity, 0.0f, 1.0f, "%.2f");
        ImGuiMCP::SliderFloat("Outline Thickness", &cfg.outline_thickness, Setting::Min_Outline_Thickness, Setting::Max_Outline_Thickness, "%.1f");

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

        if (ImGuiMCP::Button("Save to INI"))
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