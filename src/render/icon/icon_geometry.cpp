//
// Created by AmazingBuff on 2026/09/17.
//

#include "icon_geometry.h"

#include <algorithm>
#include <cmath>

namespace Amazing::HighlightLootableCorpses
{
IconGeometry icon_geometry(IconMarker const& marker, DirectX::XMFLOAT3 const& color, float width, float height)
{
    IconGeometry geometry{};
    if (!std::isfinite(marker.radius) || marker.radius <= 0.0f || !std::isfinite(marker.opacity) || marker.opacity <= 0.0f ||
        !std::isfinite(marker.tip.x) || !std::isfinite(marker.tip.y) || marker.member_count == 0 ||
        !std::isfinite(width) || !std::isfinite(height) || width <= 0.0f || height <= 0.0f ||
        !std::isfinite(color.x) || !std::isfinite(color.y) || !std::isfinite(color.z))
        return geometry;

    float const alpha = std::min(marker.opacity, 1.0f);
    DirectX::XMFLOAT4 const fill{ std::clamp(color.x, 0.0f, 1.0f), std::clamp(color.y, 0.0f, 1.0f), std::clamp(color.z, 0.0f, 1.0f), alpha };
    DirectX::XMFLOAT4 const border{ 0.04f, 0.04f, 0.04f, alpha };
    auto const triangle = [&](DirectX::XMFLOAT2 const& a, DirectX::XMFLOAT2 const& b,
        DirectX::XMFLOAT2 const& c, DirectX::XMFLOAT4 const& tint)
    {
        for (DirectX::XMFLOAT2 const& point : { a, b, c })
            geometry.vertices[geometry.count++] = { { point.x * (2.0f / width) - 1.0f, 1.0f - point.y * (2.0f / height), 0.5f }, tint };
    };
    std::size_t const arrows = marker.member_count > 1 ? 2 : 1;
    for (std::size_t arrow = 0; arrow < arrows; ++arrow)
    {
        float const y = marker.tip.y - static_cast<float>(arrow) * marker.radius * 1.8f;
        std::array<DirectX::XMFLOAT2, 3> const outer{{
            { marker.tip.x, y }, { marker.tip.x - marker.radius, y - marker.radius * 1.5f },
            { marker.tip.x + marker.radius, y - marker.radius * 1.5f } }};
        std::array<DirectX::XMFLOAT2, 3> inner;
        DirectX::XMFLOAT2 const center{ marker.tip.x, y - marker.radius };
        for (std::size_t index = 0; index < outer.size(); ++index)
            inner[index] = { center.x + (outer[index].x - center.x) * 0.72f, center.y + (outer[index].y - center.y) * 0.72f };
        triangle(inner[0], inner[1], inner[2], fill);
        for (std::size_t index = 0; index < outer.size(); ++index)
        {
            std::size_t const next = (index + 1) % outer.size();
            triangle(outer[index], outer[next], inner[next], border);
            triangle(outer[index], inner[next], inner[index], border);
        }
    }
    return geometry;
}
}
