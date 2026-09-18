//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

PLUGIN_NAMESPACE_BEGIN

class PulseTimer
{
public:
    PulseTimer(PulseTimer const&) = delete;
    PulseTimer(PulseTimer const&&) = delete;
    PulseTimer operator=(PulseTimer&) = delete;
    PulseTimer operator=(PulseTimer&&) = delete;

    static PulseTimer& instance();

    void trigger(uint32_t duration_ms);
    void reset();
    [[nodiscard]] bool active();
    [[nodiscard]] float progress();
private:
    PulseTimer();
    ~PulseTimer();
private:
    std::mutex m_mutex;
    std::chrono::steady_clock::time_point m_start_time;
    uint32_t m_duration_ms;
    bool m_active;
};

PLUGIN_NAMESPACE_END
