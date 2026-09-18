//
// Created by AmazingBuff on 2026/9/13.
//

#include "pulse_timer.h"

PLUGIN_NAMESPACE_BEGIN
PulseTimer& PulseTimer::instance()
{
    static PulseTimer s_instance;
    return s_instance;
}

void PulseTimer::trigger(uint32_t duration_ms)
{
    std::lock_guard const lock(m_mutex);
    m_start_time = std::chrono::steady_clock::now();
    m_duration_ms = duration_ms;
    m_active = true;
}

void PulseTimer::reset()
{
    std::lock_guard const lock(m_mutex);
    m_active = false;
}

bool PulseTimer::active()
{
    std::lock_guard const lock(m_mutex);
    return m_active;
}

float PulseTimer::progress()
{
    std::lock_guard const lock(m_mutex);
    if (!m_active)
        return 1.0f;

    auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_start_time).count();
    return ms > 0 ? static_cast<float>(ms) / static_cast<float>(m_duration_ms) : 0.f;
}

PulseTimer::PulseTimer() : m_duration_ms(0), m_active(false) {}

PulseTimer::~PulseTimer() = default;

PLUGIN_NAMESPACE_END
