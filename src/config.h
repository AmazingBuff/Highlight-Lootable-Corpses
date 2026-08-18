#pragma once

#include <cstdint>
#include <filesystem>

namespace Config
{
    struct Settings
    {
        bool enabled{ true };                     // 默认启用
        uint32_t hotkey{ 0x76 };             // F7
        float max_distance{ 8000.0f };            // 最大搜索距离（游戏单位，约 114 米）
        uint32_t scan_interval_ms{ 500 };    // 尸体扫描间隔
        uint32_t outline_color{ 0x00FF66 };  // 描边颜色 (RGB)
        float min_opacity{ 0.15f };               // 远处标记的最小不透明度
        float outline_thickness{ 2.0f };          // 描边线宽（像素）
        bool show_outline{ true };                // 画包围盒描边

        // 距离衰减：FadeStartDistance 内完全可见，超过后按 FadePower 指数淡出到 MinOpacity
        float fade_start_distance{ 1000.0f };  // 开始淡出的距离（游戏单位）
        float fade_power{ 2.0f };              // 淡出曲线指数（越大衰减越快）

        // 战利品筛选：开启后只显示库存命中以下任一价值分类的尸体边框
        bool loot_filter_enabled{ false };
        bool value_quest_items{ true };        // 任务物品（任务别名标记）
        bool value_keys{ true };               // 钥匙
        bool value_enchanted{ true };          // 附魔装备
        bool value_high_value{ true };         // 单件价值 >= HighValueThreshold
        float high_value_threshold{ 100.0f };  // 高价值单件阈值（金币；金币堆按枚数计）
        bool value_books{ true };              // 书籍
        int book_filter_mode{ 1 };             // 0=全部书籍 1=法术+技能书 2=仅法术书
        bool value_consumables{ true };        // 消耗品（箭矢/炼金材料/灵魂石/药水/卷轴）
        bool soul_gem_filled_only{ true };     // 灵魂石仅算已填充（需实例条目带灵魂等级）
    };

    [[nodiscard]] Settings const& get() noexcept;
    // 游戏主线程（MCP 菜单）修改用；渲染线程经 get() 无锁读取，与 set_enabled 同模式
    [[nodiscard]] Settings& get_mutable() noexcept;

    [[nodiscard]] bool is_enabled() noexcept;
    void set_enabled(bool a_enabled) noexcept;
    void save_enabled() noexcept;

    void load() noexcept;
    void save() noexcept;            // 全量写回 INI（含全部选项说明）
    void reset_defaults() noexcept;  // 恢复默认值并写回
    [[nodiscard]] std::filesystem::path get_ini_path() noexcept;
}
