#pragma once

#include <cstdint>
#include <filesystem>

namespace Config
{
    struct Settings
    {
        bool enabled{ true };                     // 默认启用
        std::uint32_t hotkey{ 0x76 };             // F7
        float max_distance{ 8000.0f };            // 最大搜索距离（游戏单位，约 114 米）
        std::uint32_t scan_interval_ms{ 500 };    // 尸体扫描间隔
        std::uint32_t outline_color{ 0x00FF66 };  // 描边颜色 (RGB)
        float glow_alpha{ 0.30f };                // 发光强度
        float min_opacity{ 0.15f };               // 远处标记的最小不透明度
        float outline_thickness{ 2.0f };          // 描边线宽（像素）
        bool show_outline{ true };                // 画包围盒描边（默认仅边框）
        bool show_glow{ false };                  // 画发光填充（默认关闭）
        bool show_center_dot{ false };            // 画中心点（默认关闭）
        bool show_indicator{ true };              // 画右上角开关指示点

        // 距离衰减：FadeStartDistance 内完全可见，超过后按 FadePower 指数淡出到 MinOpacity
        float fade_start_distance{ 1000.0f };  // 开始淡出的距离（游戏单位）
        float fade_power{ 2.0f };              // 淡出曲线指数（越大衰减越快）
    };

    [[nodiscard]] Settings const& get() noexcept;

    [[nodiscard]] bool is_enabled() noexcept;
    void set_enabled(bool a_enabled) noexcept;
    void save_enabled() noexcept;

    void load() noexcept;
    [[nodiscard]] std::filesystem::path get_ini_path() noexcept;
}
