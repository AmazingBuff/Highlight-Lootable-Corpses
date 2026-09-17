//
// Created by AmazingBuff on 2026/09/17.
//

#pragma once

#include "mask_types.h"

#include "Plugin.h"

#include <DirectXMath.h>

#include <cmath>

MASK_NAMESPACE_BEGIN

// Column-vector projection: replace only the z row so z/w = near / view distance.
// The w gradient accounts for a uniform scale in the engine projection. Applying
// this before World/palette multiplication preserves homogeneous skin weights.
inline bool make_private_depth_projection(DirectX::XMFLOAT4X4& projection, float near_plane, bool orthographic)
{
    if (orthographic || !std::isfinite(near_plane) || near_plane <= 0.0f)
        return false;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            if (!std::isfinite(projection.m[row][col]))
                return false;
    float const scale = std::hypot(projection._41, projection._42, projection._43);
    float const near_clip = near_plane * scale;
    if (!std::isfinite(near_clip) || near_clip <= 0.0f)
        return false;
    projection._31 = 0.0f;
    projection._32 = 0.0f;
    projection._33 = 0.0f;
    projection._34 = near_clip;
    return true;
}

MASK_NAMESPACE_END
