//
// Created by AmazingBuff on 2026/9/14.
//

#include "render_util.h"

PLUGIN_NAMESPACE_BEGIN


uint32_t Color::encode() const
{
    return (static_cast<uint32_t>(m_a * 255.0f) << 24) |
           (static_cast<uint32_t>(m_r * 255.0f) << 16) |
           (static_cast<uint32_t>(m_g * 255.0f) << 8) |
            static_cast<uint32_t>(m_b * 255.0f);
}

void Color::decode(uint32_t v)
{
    m_r = static_cast<float>((v >> 16) & 0xFF) / 255.0f;
    m_g = static_cast<float>((v >> 8) & 0xFF) / 255.0f;
    m_b = static_cast<float>(v & 0xFF) / 255.0f;
    m_a = static_cast<float>((v >> 24) & 0xFF) / 255.0f;
}



PLUGIN_NAMESPACE_END