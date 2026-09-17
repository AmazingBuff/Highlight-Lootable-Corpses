//
// Created by AmazingBuff on 2026/09/17.
//

#pragma once

#include <DirectXMath.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace Amazing::HighlightLootableCorpses
{
struct IconCandidate
{
    std::uint32_t form_id;
    DirectX::XMFLOAT3 anchor;
    DirectX::XMFLOAT2 tip;
    float distance;
    float opacity;
};

struct IconMarker
{
    std::uint32_t representative;
    std::size_t member_count;
    DirectX::XMFLOAT2 tip;
    float radius;
    float opacity;
};

[[nodiscard]] DirectX::XMFLOAT3 icon_anchor(DirectX::XMFLOAT3 const& anchor,
    DirectX::XMFLOAT3 const& minimum, DirectX::XMFLOAT3 const& maximum);
[[nodiscard]] float icon_radius(float base_radius, float distance, float max_distance, bool clustered);
[[nodiscard]] bool icon_screen_tip(float px, float py, float depth, float width, float height,
    DirectX::XMFLOAT2& tip);
[[nodiscard]] bool icon_clip_tip(DirectX::XMFLOAT4 const& clip, float width, float height, DirectX::XMFLOAT2& tip);

class IconLayout
{
public:
    IconLayout();

    [[nodiscard]] std::vector<IconMarker> update(std::span<IconCandidate const> candidates,
        float base_radius, float max_distance, float width, float height, std::size_t marker_limit);
    void reset();

private:
    std::unordered_map<std::uint32_t, std::uint32_t> m_membership;
    float m_width;
    float m_height;
};
}
