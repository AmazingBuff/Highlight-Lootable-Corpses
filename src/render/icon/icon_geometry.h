//
// Created by AmazingBuff on 2026/09/17.
//

#pragma once

#include "icon_layout.h"

#include <DirectXMath.h>

#include <array>
#include <cstddef>

namespace Amazing::HighlightLootableCorpses
{
struct IconVertex
{
    DirectX::XMFLOAT3 pos;
    DirectX::XMFLOAT4 color;
};

inline constexpr size_t Icon_Arrow_Vertex_Count = 21;
inline constexpr size_t Icon_Marker_Vertex_Count = 2 * Icon_Arrow_Vertex_Count;

struct IconGeometry
{
    std::array<IconVertex, Icon_Marker_Vertex_Count> vertices;
    size_t count;
};

[[nodiscard]] IconGeometry icon_geometry(IconMarker const& marker, DirectX::XMFLOAT3 const& color,
    float width, float height);
}
