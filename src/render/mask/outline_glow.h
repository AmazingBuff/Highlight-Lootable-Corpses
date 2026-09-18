//
// Created by AmazingBuff on 2026/9/18.
//

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

PLUGIN_NAMESPACE_BEGIN

namespace Mask::Glow
{

inline constexpr int Min_Thickness = 1;
inline constexpr int Max_Thickness = 5;
inline constexpr int Max_Radius = 18;
inline constexpr std::size_t Kernel_Slot_Count = (Max_Radius + 4) / 4;

struct KernelProfile
{
    int thickness;
    int radius;
    float halo_sigma;
    float core_sigma;
    std::array<float, Max_Radius + 1> narrow;
    std::array<float, Max_Radius + 1> wide;
};

[[nodiscard]] inline KernelProfile make_kernel_profile(int thickness) noexcept
{
    KernelProfile profile{};
    profile.thickness = std::clamp(thickness, Min_Thickness, Max_Thickness);
    profile.radius = 3 * profile.thickness + 3;
    profile.halo_sigma = static_cast<float>(profile.radius) / 3.0f;
    profile.core_sigma = 0.45f + 0.20f * static_cast<float>(profile.thickness);

    float narrow_sum = 0.0f;
    float wide_sum = 0.0f;
    for (int distance = 0; distance <= profile.radius; ++distance)
    {
        float const distance_f = static_cast<float>(distance);
        float const narrow_value = std::exp(-0.5f * distance_f * distance_f / (profile.core_sigma * profile.core_sigma));
        float const wide_value = std::exp(-0.5f * distance_f * distance_f / (profile.halo_sigma * profile.halo_sigma));
        profile.narrow[distance] = narrow_value;
        profile.wide[distance] = wide_value;
        narrow_sum += distance == 0 ? narrow_value : 2.0f * narrow_value;
        wide_sum += distance == 0 ? wide_value : 2.0f * wide_value;
    }

    for (int distance = 0; distance <= profile.radius; ++distance)
    {
        profile.narrow[distance] /= narrow_sum;
        profile.wide[distance] /= wide_sum;
    }
    return profile;
}

} // namespace Mask::Glow

PLUGIN_NAMESPACE_END
