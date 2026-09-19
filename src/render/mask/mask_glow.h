//
// Created by AmazingBuff on 2026/9/18.
//

#pragma once

#include "mask_types.h"

#include "config/config.h"

MASK_NAMESPACE_BEGIN

namespace Glow
{

inline constexpr int Kernel_Factor = 3;
inline constexpr int Kernel_Max_Radius = 3 * Setting::Max_Outline_Thickness;
inline constexpr std::size_t Kernel_Slot_Count = (Kernel_Max_Radius + 4) / 4;

struct KernelProfile
{
    int thickness;
    int radius;
    float halo_sigma;
    float core_sigma;
    std::array<float, Kernel_Max_Radius + 1> narrow;
    std::array<float, Kernel_Max_Radius + 1> wide;
};

[[nodiscard]] inline KernelProfile make_kernel_profile(int thickness) noexcept
{
    KernelProfile profile{};
    profile.thickness = thickness;
    profile.radius = Kernel_Factor * profile.thickness;
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

MASK_NAMESPACE_END
