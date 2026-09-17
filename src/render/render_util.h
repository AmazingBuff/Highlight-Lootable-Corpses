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
    [[nodiscard]] uint32_t encode() const;

    // decode from argb
    void decode(uint32_t v);

    float& r() { return m_r; }
    float& g() { return m_g; }
    float& b() { return m_b; }
    float& a() { return m_a; }

    [[nodiscard]] const float& r() const { return m_r; }
    [[nodiscard]] const float& g() const { return m_g; }
    [[nodiscard]] const float& b() const { return m_b; }
    [[nodiscard]] const float& a() const { return m_a; }
private:
    float m_r, m_g, m_b, m_a;
};


class Rect
{
public:
    Rect() : m_left(0.0f), m_right(0.0f), m_bottom(0.0f), m_top(0.0f) {}

    [[nodiscard]] float width() const { return m_right - m_left; }
    [[nodiscard]] float height() const { return m_bottom - m_left; }

    float& left() { return m_left; }
    float& right() { return m_right; }
    float& bottom() { return m_bottom; }
    float& top() { return m_top; }

    [[nodiscard]] const float& left() const { return m_left; }
    [[nodiscard]] const float& right() const { return m_right; }
    [[nodiscard]] const float& bottom() const { return m_bottom; }
    [[nodiscard]] const float& top() const { return m_top; }
private:
    float m_left, m_right, m_bottom, m_top;
};

bool project(RE::NiCamera* camera, RE::NiPoint3 const& point, float width, float height, float& px, float& py, float& depth);

bool world_to_screen(RE::NiCamera* camera, RE::BSGraphics::ViewData const* view_data, RE::NiPoint3 const& point, float width, float height, float& px, float& py, float& depth);

PLUGIN_NAMESPACE_END