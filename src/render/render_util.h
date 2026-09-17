//
// Created by AmazingBuff on 2026/9/14.
//

#pragma once

#include "Plugin.h"

#include <cstdint>

namespace RE
{
    class NiCamera;
    class NiPoint3;

    namespace BSGraphics
    {
        struct ViewData;
    }
}

PLUGIN_NAMESPACE_BEGIN

class Color
{
public:
    Color() : m_r(0.0f), m_g(0.0f), m_b(0.0f), m_a(0.0f) {}

    // encode to argb
    [[nodiscard]] uint32_t encode() const noexcept;

    // decode from argb
    void decode(uint32_t v);

    float& r() noexcept { return m_r; }
    float& g() noexcept { return m_g; }
    float& b() noexcept { return m_b; }
    float& a() noexcept { return m_a; }

    [[nodiscard]] float const& r() const noexcept { return m_r; }
    [[nodiscard]] float const& g() const noexcept { return m_g; }
    [[nodiscard]] float const& b() const noexcept { return m_b; }
    [[nodiscard]] float const& a() const noexcept { return m_a; }
private:
    float m_r, m_g, m_b, m_a;
};


class Rect
{
public:
    Rect() : m_left(0.0f), m_right(0.0f), m_bottom(0.0f), m_top(0.0f) {}

    [[nodiscard]] float width() const noexcept { return m_right - m_left; }
    [[nodiscard]] float height() const noexcept { return m_bottom - m_left; }

    float& left() noexcept { return m_left; }
    float& right() noexcept { return m_right; }
    float& bottom() noexcept { return m_bottom; }
    float& top() noexcept { return m_top; }

    [[nodiscard]] float const& left() const noexcept { return m_left; }
    [[nodiscard]] float const& right() const noexcept { return m_right; }
    [[nodiscard]] float const& bottom() const noexcept { return m_bottom; }
    [[nodiscard]] float const& top() const noexcept { return m_top; }
private:
    float m_left, m_right, m_bottom, m_top;
};

[[nodiscard]] bool project(RE::NiCamera* camera, RE::NiPoint3 const& point, float width, float height, float& px, float& py, float& depth);

[[nodiscard]] bool world_to_screen(RE::NiCamera* camera, RE::BSGraphics::ViewData const* view_data, RE::NiPoint3 const& point, float width, float height, float& px, float& py, float& depth);

PLUGIN_NAMESPACE_END