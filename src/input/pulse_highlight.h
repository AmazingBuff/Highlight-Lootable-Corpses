//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>

PLUGIN_NAMESPACE_BEGIN

// 消退模式（HotkeyMode::e_pulse）的脉冲状态：按热键记录触发时刻，渲染端按
// elapsed/duration 渐隐。单实例——同一时刻只有一个脉冲在途（重复按键刷新时刻）。
//
// 线程模型：trigger/reset 由输入事件线程调用，active/elapsed_ms 由渲染线程
//（Present 回调）调用；内部互斥锁保护时间戳。
class PulseHighlight
{
public:
    PulseHighlight() = delete;
    ~PulseHighlight() = delete;
    PulseHighlight(PulseHighlight const&) = delete;
    PulseHighlight(PulseHighlight const&&) = delete;
    PulseHighlight operator=(PulseHighlight&) = delete;
    PulseHighlight operator=(PulseHighlight&&) = delete;

    // 记录/刷新脉冲触发时刻并快照本次脉冲的时长（重复按键重置渐隐进度并采用
    // 当前配置值——时长修改只影响之后触发的脉冲，符合"下次消退才应用"）
    static void trigger(std::uint32_t a_duration_ms);

    // 结束当前脉冲（模式切回常亮时调用）
    static void reset();

    // 脉冲是否在途（未过期）
    [[nodiscard]] static bool active();

    // 已流逝毫秒数；不在途返回 0
    [[nodiscard]] static std::uint32_t elapsed_ms();

    // 本次脉冲的总时长（trigger 时快照的配置值）；不在途返回 0
    [[nodiscard]] static std::uint32_t duration_ms();

private:
    struct State
    {
        std::mutex mutex;
        std::chrono::steady_clock::time_point start_time{};
        std::uint32_t duration_ms = 0;
        bool active = false;
    };
    static State& state();
};

PLUGIN_NAMESPACE_END
