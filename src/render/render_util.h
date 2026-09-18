//
// Created by AmazingBuff on 2026/9/14.
//

#pragma once

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


[[nodiscard]] bool project(RE::NiCamera* camera, DirectX::XMFLOAT3 const& point, float width, float height, float& px, float& py, float& depth);

[[nodiscard]] bool world_to_screen(RE::NiCamera* camera, RE::BSGraphics::ViewData const* view_data, DirectX::XMFLOAT3 const& point, float width, float height, float& px, float& py, float& depth);

[[nodiscard]] DirectX::XMFLOAT3 render_cast(RE::NiPoint3 const& p);
[[nodiscard]] RE::NiPoint3 skyrim_cast(DirectX::XMFLOAT3 const& p);

PLUGIN_NAMESPACE_END