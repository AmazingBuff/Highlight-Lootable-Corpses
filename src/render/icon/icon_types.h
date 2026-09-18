//
// Created by AmazingBuff on 2026/09/17.
//

#pragma once

#define ICON_NAMESPACE_BEGIN PLUGIN_NAMESPACE_BEGIN namespace Icon {
#define ICON_NAMESPACE_END PLUGIN_NAMESPACE_END }

ICON_NAMESPACE_BEGIN

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

struct IconCandidate
{
    uint32_t form_id;
    DirectX::XMFLOAT3 anchor;
    DirectX::XMFLOAT2 tip;
    float distance;
    float opacity;
};

struct IconMarker
{
    uint32_t representative;
    size_t member_count;
    DirectX::XMFLOAT2 tip;
    float radius;
    float opacity;
};

ICON_NAMESPACE_END