//
// Created by AmazingBuff on 2026/9/18.
//

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include <DirectXMath.h>

PLUGIN_NAMESPACE_BEGIN

namespace Mask::ROI
{

inline constexpr int Raster_Margin = 2;

struct Sphere
{
    double center_x;
    double center_y;
    double center_z;
    double radius;
};

struct Viewport
{
    int32_t width;
    int32_t height;
};

struct Rect
{
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

enum class RegionKind : uint8_t
{
    e_full,
    e_empty,
    e_rect
};

struct Region
{
    RegionKind kind;
    Rect rect;
};

[[nodiscard]] inline bool finite(double value) noexcept
{
    return std::isfinite(value);
}

[[nodiscard]] inline Region full_region() noexcept
{
    return { RegionKind::e_full, {} };
}

[[nodiscard]] inline Region empty_region() noexcept
{
    return { RegionKind::e_empty, {} };
}

[[nodiscard]] inline Region make_region(std::span<Sphere const> spheres, DirectX::XMFLOAT4X4 const& matrix, Viewport viewport) noexcept
{
    if (viewport.width <= 0 || viewport.height <= 0 || spheres.empty())
        return full_region();

    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t col = 0; col < 4; ++col)
            if (!finite(static_cast<double>(matrix.m[row][col])))
                return full_region();

    double min_x = std::numeric_limits<double>::infinity();
    double min_y = std::numeric_limits<double>::infinity();
    double max_x = -std::numeric_limits<double>::infinity();
    double max_y = -std::numeric_limits<double>::infinity();
    for (Sphere const& sphere : spheres)
    {
        if (!finite(sphere.center_x) || !finite(sphere.center_y) || !finite(sphere.center_z) ||
            !finite(sphere.radius) || sphere.radius <= 0.0)
            return full_region();

        for (int z = -1; z <= 1; z += 2)
            for (int y = -1; y <= 1; y += 2)
                for (int x = -1; x <= 1; x += 2)
                {
                    double const point[4]{
                        sphere.center_x + static_cast<double>(x) * sphere.radius,
                        sphere.center_y + static_cast<double>(y) * sphere.radius,
                        sphere.center_z + static_cast<double>(z) * sphere.radius,
                        1.0
                    };
                    double clip[4]{};
                    for (std::size_t row = 0; row < 4; ++row)
                        for (std::size_t col = 0; col < 4; ++col)
                            clip[row] += static_cast<double>(matrix.m[row][col]) * point[col];
                    if (!finite(clip[0]) || !finite(clip[1]) || !finite(clip[2]) || !finite(clip[3]) ||
                        !(clip[3] > 0.0) || !(clip[3] > clip[2]))
                        return full_region();

                    double const pixel_x = (clip[0] / clip[3] * 0.5 + 0.5) * static_cast<double>(viewport.width);
                    double const pixel_y = (1.0 - (clip[1] / clip[3] * 0.5 + 0.5)) * static_cast<double>(viewport.height);
                    if (!finite(pixel_x) || !finite(pixel_y))
                        return full_region();
                    min_x = std::min(min_x, pixel_x);
                    min_y = std::min(min_y, pixel_y);
                    max_x = std::max(max_x, pixel_x);
                    max_y = std::max(max_y, pixel_y);
                }
    }

    double const left = std::floor(min_x) - Raster_Margin;
    double const top = std::floor(min_y) - Raster_Margin;
    double const right = std::ceil(max_x) + Raster_Margin;
    double const bottom = std::ceil(max_y) + Raster_Margin;
    if (!(right > 0.0) || !(bottom > 0.0) || left >= static_cast<double>(viewport.width) || top >= static_cast<double>(viewport.height))
        return empty_region();

    Rect const rect{
        .left = left <= 0.0 ? 0 : static_cast<int32_t>(std::floor(left)),
        .top = top <= 0.0 ? 0 : static_cast<int32_t>(std::floor(top)),
        .right = right >= static_cast<double>(viewport.width) ? viewport.width : static_cast<int32_t>(std::ceil(right)),
        .bottom = bottom >= static_cast<double>(viewport.height) ? viewport.height : static_cast<int32_t>(std::ceil(bottom)),
    };
    if (rect.right <= rect.left || rect.bottom <= rect.top)
        return empty_region();
    return { RegionKind::e_rect, rect };
}

[[nodiscard]] inline Region expand(Region region, int radius, bool horizontal, bool vertical, Viewport viewport) noexcept
{
    if (region.kind != RegionKind::e_rect)
        return region;
    if (radius < 0 || viewport.width <= 0 || viewport.height <= 0)
        return full_region();

    int64_t const left = static_cast<int64_t>(region.rect.left) - (horizontal ? radius : 0);
    int64_t const top = static_cast<int64_t>(region.rect.top) - (vertical ? radius : 0);
    int64_t const right = static_cast<int64_t>(region.rect.right) + (horizontal ? radius : 0);
    int64_t const bottom = static_cast<int64_t>(region.rect.bottom) + (vertical ? radius : 0);
    Rect const expanded{
        .left = static_cast<int32_t>(std::clamp<int64_t>(left, 0, viewport.width)),
        .top = static_cast<int32_t>(std::clamp<int64_t>(top, 0, viewport.height)),
        .right = static_cast<int32_t>(std::clamp<int64_t>(right, 0, viewport.width)),
        .bottom = static_cast<int32_t>(std::clamp<int64_t>(bottom, 0, viewport.height)),
    };
    if (expanded.right <= expanded.left || expanded.bottom <= expanded.top)
        return empty_region();
    return { RegionKind::e_rect, expanded };
}

} // namespace Mask::ROI

PLUGIN_NAMESPACE_END
