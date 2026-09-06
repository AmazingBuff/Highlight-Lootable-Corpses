#include "pch.h"
#include "ui_menu.h"
#include "config.h"
#include "corpse_finder.h"
#include "input.h"
#include "loot_filter.h"

// SKSE-MCP（header-only）通过 GetProcAddress 动态加载 SKSEMenuFramework.dll，
// 无链接依赖；头文件自身带多种 /W4 告警（C4996 弃用 codecvt、C5054 跨枚举 |、
// C4099 struct/class 混用、C4267/C4244 隐式转换），项目 /WX 下需静默。
#pragma warning(push)
#pragma warning(disable: 4996 5054 4099 4267 4244 4061 4062)
#include <SKSEMCP/utils.hpp>
#pragma warning(pop)

namespace
{
    // Windows VK 码 -> 简短名称；未覆盖的键显示十六进制
    std::string hotkey_name(std::uint32_t a_vk)
    {
        if (a_vk == 0)
            return "None";

        switch (a_vk)
        {
        case 0x01: return "LMB";
        case 0x02: return "RMB";
        case 0x04: return "MMB";
        case 0x05: return "Mouse 4";
        case 0x06: return "Mouse 5";
        }

        static const char* const kNames[] = {
            "Backspace", "Tab", nullptr, nullptr, nullptr, "Enter", nullptr, nullptr,   // 0x08-0x0F
            "Shift", "Ctrl", "Alt", "Pause", "Caps", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "Esc", nullptr, nullptr, nullptr, nullptr,   // 0x10-0x1F
            "Space", "PgUp", "PgDn", "End", "Home", "Left", "Up", "Right", "Down", nullptr, nullptr, nullptr, nullptr, "Ins", "Del",   // 0x20-0x2E
        };
        if (a_vk >= 0x08 && a_vk <= 0x2E)
        {
            char const* name = kNames[a_vk - 0x08];
            if (name)
                return name;
        }
        if (a_vk >= 0x30 && a_vk <= 0x39)
            return std::string(1, static_cast<char>(a_vk));                        // 0-9
        if (a_vk >= 0x41 && a_vk <= 0x5A)
            return std::string(1, static_cast<char>(a_vk));                        // A-Z
        if (a_vk >= 0x60 && a_vk <= 0x69)
            return fmt::format("Num {}", a_vk - 0x60);                             // 小键盘 0-9
        if (a_vk >= 0x70 && a_vk <= 0x87)
            return fmt::format("F{}", a_vk - 0x6F);                                // F1-F24
        return fmt::format("0x{:02X}", a_vk);
    }

    // MCP 菜单回调：游戏主线程执行（框架在 imgui 帧内调用），
    // 直接读写 Config 设置；修改即时生效，渲染线程无锁读取（与 set_enabled 同模式）。
    void __stdcall render_settings()
    {
        Config::Settings& s = Config::get_mutable();

        // 总开关：必须经 set_enabled 同步 g_enabled（渲染线程读它），直接改字段无效
        bool enabled = s.enabled;
        if (ImGuiMCP::Checkbox("Enabled", &enabled))
            Config::set_enabled(enabled);

        // 热键：点击进入捕获态，此后按下的第一个键（键盘或鼠标）即绑定为切换
        // 热键——全量绑定，ESC/F1 等会触发面板开合的键也可绑定（面板可能被
        // 关闭，绑定仍生效）；再次点击按钮取消，5 秒无按键自动取消。热键切换
        // enabled 走 set_enabled 同步 g_enabled，本菜单的 Enabled 复选框每帧从
        // g_settings 读值——热键与 UI 状态自动双向同步。
        static bool s_rebinding = false;
        std::uint32_t rebind_vk = 0;
        if (s_rebinding && Input::take_rebind_result(rebind_vk))
        {
            s_rebinding = false;
            if (rebind_vk != 0)
            {
                s.hotkey = rebind_vk;
                Config::save();  // 全量写回 INI
            }
        }
        std::string const label = s_rebinding
            ? std::string("Press any key...")
            : fmt::format("Hotkey: {}", hotkey_name(s.hotkey));
        if (ImGuiMCP::Button(label.c_str()))
        {
            if (s_rebinding)
            {
                Input::cancel_rebind();
                s_rebinding = false;
            }
            else
            {
                Input::begin_rebind();
                s_rebinding = true;
            }
        }

        ImGuiMCP::SliderFloat("Max Search Distance", &s.max_distance, 500.0f, 10000.0f, "%.0f");
        int scan_ms = static_cast<int>(s.scan_interval_ms);
        if (ImGuiMCP::SliderInt("Scan Interval (ms)", &scan_ms, 100, 5000))
            s.scan_interval_ms = static_cast<std::uint32_t>(scan_ms);

        // 描边颜色：uint32 RGB -> float[3]（sRGB 值，与渲染端提取逻辑一致）
        float color[3] = {
            static_cast<float>((s.outline_color >> 16) & 0xFF) / 255.0f,
            static_cast<float>((s.outline_color >> 8) & 0xFF) / 255.0f,
            static_cast<float>(s.outline_color & 0xFF) / 255.0f,
        };
        if (ImGuiMCP::ColorEdit3("Outline Color", color))
        {
            s.outline_color =
                (static_cast<std::uint32_t>(color[0] * 255.0f) << 16) |
                (static_cast<std::uint32_t>(color[1] * 255.0f) << 8) |
                static_cast<std::uint32_t>(color[2] * 255.0f);
        }

        ImGuiMCP::SliderFloat("Min Opacity", &s.min_opacity, 0.0f, 1.0f, "%.2f");
        ImGuiMCP::SliderFloat("Outline Thickness", &s.outline_thickness, 1.0f, 8.0f, "%.1f");

        ImGuiMCP::SliderFloat("Fade Start Distance", &s.fade_start_distance, 0.0f, s.max_distance, "%.0f");
        ImGuiMCP::SliderFloat("Fade Power", &s.fade_power, 0.1f, 8.0f, "%.1f");

        ImGuiMCP::Separator();

        // 战利品筛选：只显示库存命中以下任一分类的尸体（扫描期评估，过滤即时生效）。
        // 总开关关闭时其下参数在 UI 中灰显禁用（改总开关才有意义）
        ImGuiMCP::Checkbox("Enable Loot Filter", &s.loot_filter_enabled);
        ImGuiMCP::BeginDisabled(!s.loot_filter_enabled);
        ImGuiMCP::Checkbox("Quest Items", &s.value_quest_items);
        ImGuiMCP::Checkbox("Keys", &s.value_keys);
        ImGuiMCP::Checkbox("Enchanted Gear", &s.value_enchanted);
        ImGuiMCP::Checkbox("High-Value Items", &s.value_high_value);
        ImGuiMCP::SliderFloat("High Value Threshold", &s.high_value_threshold, 0.0f, 1000.0f, "%.0f");
        ImGuiMCP::Checkbox("Books", &s.value_books);
        static constexpr char const* kBookModes[] = { "All Books", "Spell & Skill Books", "Spell Books Only" };
        ImGuiMCP::Combo("Book Mode", &s.book_filter_mode, kBookModes, 3);
        ImGuiMCP::Checkbox("Consumables", &s.value_consumables);
        ImGuiMCP::EndDisabled();

        ImGuiMCP::Separator();

        // 实时状态：帮助验证距离衰减（fade 只作用于超过 Fade Start 距离的尸体）
        std::vector<CorpseFinder::CorpseEntry> const corpses = CorpseFinder::snapshot();
        float nearest = 0.0f;
        for (auto const& corpse : corpses)
            nearest = nearest == 0.0f ? corpse.distance : std::min(nearest, corpse.distance);

        ImGuiMCP::Text("Corpses: %d | Nearest: %.0f units", static_cast<int>(corpses.size()), nearest);
        if (s.loot_filter_enabled)
        {
            std::uint16_t const mask = LootFilter::enabled_category_mask();
            int visible = 0;
            for (auto const& corpse : corpses)
            {
                if ((corpse.loot_categories & mask) != 0)
                    ++visible;
            }
            ImGuiMCP::Text("Visible: %d / %d corpses", visible, static_cast<int>(corpses.size()));
        }

        if (ImGuiMCP::Button("Save to INI"))
            Config::save();

        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button("Reset to Defaults"))
            Config::reset_defaults();
    }
}

namespace UiMenu
{
    bool is_menu_open()
    {
        return SKSEMenuFramework::IsInstalled() && SKSEMenuFramework::IsAnyBlockingWindowOpened();
    }

    void register_menus()
    {
        // kDataLoaded/kNewGame/kPostLoadGame 都会走到这里，只注册一次
        static bool s_registered = false;
        if (s_registered)
            return;

        // SKSE Menu Framework 是可选依赖：未安装时插件其余功能（描边、扫描、
        // INI 配置、热键）完全可用，仅游戏内设置菜单不可用
        if (!SKSEMenuFramework::IsInstalled())
        {
            logger::warn("SKSE Menu Framework (SKSEMenuFramework.dll) not installed, in-game settings menu disabled");
            return;
        }

        SKSEMenuFramework::SetSection("Highlight Lootable Corpses");
        SKSEMenuFramework::AddSectionItem("Settings", render_settings);
        s_registered = true;

        logger::info(
            "Registered Highlight Lootable Corpses settings page (SKSE Menu Framework v{:.2f})",
            SKSEMenuFramework::GetMenuFrameworkVersion());
    }
}
