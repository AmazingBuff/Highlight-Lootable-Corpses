#include "pch.h"
#include "ui_menu.h"
#include "config.h"
#include "corpse_finder.h"
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
    // MCP 菜单回调：游戏主线程执行（框架在 imgui 帧内调用），
    // 直接读写 Config 设置；修改即时生效，渲染线程无锁读取（与 set_enabled 同模式）。
    void __stdcall render_settings()
    {
        Config::Settings& s = Config::get_mutable();

        // 总开关：必须经 set_enabled 同步 g_enabled（渲染线程读它），直接改字段无效
        bool enabled = s.enabled;
        if (ImGuiMCP::Checkbox("Enabled", &enabled))
            Config::set_enabled(enabled);

        ImGuiMCP::SliderFloat("Max Search Distance", &s.max_distance, 500.0f, 50000.0f, "%.0f");
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

        ImGuiMCP::Checkbox("Show Outline", &s.show_outline);
        // 剪影模式的网格在扫描期采集，切换后最多 ScanIntervalMs 生效
        static constexpr char const* s_outline_modes[] = { "Bounding Box", "Model Silhouette" };
        ImGuiMCP::Combo("Outline Mode", &s.outline_mode, s_outline_modes, 2);

        ImGuiMCP::SliderFloat("Fade Start Distance", &s.fade_start_distance, 0.0f, s.max_distance, "%.0f");
        ImGuiMCP::SliderFloat("Fade Power", &s.fade_power, 0.1f, 8.0f, "%.1f");

        ImGuiMCP::Separator();

        // 战利品筛选：只显示库存命中以下任一分类的尸体（扫描期评估，过滤即时生效）
        ImGuiMCP::Checkbox("Enable Loot Filter", &s.loot_filter_enabled);
        ImGuiMCP::Checkbox("Quest Items", &s.value_quest_items);
        ImGuiMCP::Checkbox("Keys", &s.value_keys);
        ImGuiMCP::Checkbox("Enchanted Gear", &s.value_enchanted);
        ImGuiMCP::Checkbox("High-Value Items", &s.value_high_value);
        ImGuiMCP::SliderFloat("High Value Threshold", &s.high_value_threshold, 10.0f, 10000.0f, "%.0f");
        ImGuiMCP::Checkbox("Books", &s.value_books);
        static constexpr char const* kBookModes[] = { "All Books", "Spell & Skill Books", "Spell Books Only" };
        ImGuiMCP::Combo("Book Mode", &s.book_filter_mode, kBookModes, 3);
        ImGuiMCP::Checkbox("Consumables", &s.value_consumables);
        ImGuiMCP::Checkbox("Filled Soul Gems Only", &s.soul_gem_filled_only);

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
        ImGuiMCP::Text("ESP Toggle Hotkey: 0x%02X (edit in INI)", s.hotkey);

        if (ImGuiMCP::Button("Save to INI"))
            Config::save();

        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button("Reset to Defaults"))
            Config::reset_defaults();
    }
}

namespace UiMenu
{
    void register_menus()
    {
        if (!SKSEMenuFramework::IsInstalled())
        {
            logger::warn("SKSE Menu Framework (SKSEMenuFramework.dll) not installed, in-game settings menu disabled");
            return;
        }

        SKSEMenuFramework::SetSection("CorpseESP");
        SKSEMenuFramework::AddSectionItem("Settings", render_settings);

        logger::info(
            "Registered CorpseESP settings page (SKSE Menu Framework v{:.2f})",
            SKSEMenuFramework::GetMenuFrameworkVersion());
    }
}
