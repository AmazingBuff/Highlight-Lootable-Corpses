//
// Created by AmazingBuff on 2026/9/13.
//

#include "pulse_highlight.h"

#include <chrono>
#include <mutex>

PLUGIN_NAMESPACE_BEGIN

void PulseHighlight::trigger(std::uint32_t a_duration_ms)
{
    State& s = state();
    std::lock_guard const lock(s.mutex);
    s.start_time = std::chrono::steady_clock::now();
    s.duration_ms = a_duration_ms;
    s.active = true;
}

void PulseHighlight::reset()
{
    State& s = state();
    std::lock_guard const lock(s.mutex);
    s.active = false;
}

bool PulseHighlight::active()
{
    State& s = state();
    std::lock_guard const lock(s.mutex);
    return s.active;
}

std::uint32_t PulseHighlight::elapsed_ms()
{
    State& s = state();
    std::lock_guard const lock(s.mutex);
    if (!s.active)
        return 0;

    auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s.start_time).count();
    return ms > 0 ? static_cast<std::uint32_t>(ms) : 0;
}

std::uint32_t PulseHighlight::duration_ms()
{
    State& s = state();
    std::lock_guard const lock(s.mutex);
    return s.active ? s.duration_ms : 0;
}

PulseHighlight::State& PulseHighlight::state()
{
    static State s_state;
    return s_state;
}

PLUGIN_NAMESPACE_END
