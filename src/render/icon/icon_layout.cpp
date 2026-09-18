//
// Created by AmazingBuff on 2026/09/17.
//

#include "icon_layout.h"

ICON_NAMESPACE_BEGIN

namespace
{
    constexpr float Join_World_Distance = 180.0f;
    constexpr float Join_Height = 64.0f;
    constexpr float Join_Screen_Minimum = 24.0f;
    constexpr float Join_Screen_Gap = 8.0f;
    constexpr float Exit_Factor = 1.25f;
    constexpr float Tip_Gap = 6.0f;

    bool finite(DirectX::XMFLOAT3 const& point)
    {
        return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
    }

    double distance_squared(DirectX::XMFLOAT3 const& a, DirectX::XMFLOAT3 const& b)
    {
        double const x = static_cast<double>(a.x) - b.x;
        double const y = static_cast<double>(a.y) - b.y;
        double const z = static_cast<double>(a.z) - b.z;
        return x * x + y * y + z * z;
    }

    struct Member
    {
        IconCandidate candidate;
        float radius;
        uint32_t previous_group;
    };

    bool compatible(Member const& a, Member const& b)
    {
        float const factor = a.previous_group != 0 && a.previous_group == b.previous_group ? Exit_Factor : 1.0f;
        double const world_limit = Join_World_Distance * factor;
        double const screen_limit = std::max(Join_Screen_Minimum, 2.0f * std::max(a.radius, b.radius) + Join_Screen_Gap) * factor;
        double const dx = static_cast<double>(a.candidate.tip.x) - b.candidate.tip.x;
        double const dy = static_cast<double>(a.candidate.tip.y) - b.candidate.tip.y;
        return distance_squared(a.candidate.anchor, b.candidate.anchor) <= world_limit * world_limit &&
            std::abs(static_cast<double>(a.candidate.anchor.z) - b.candidate.anchor.z) <= Join_Height * factor &&
            dx * dx + dy * dy <= screen_limit * screen_limit;
    }
}

DirectX::XMFLOAT3 icon_anchor(DirectX::XMFLOAT3 const& minimum, DirectX::XMFLOAT3 const& maximum)
{
    return { std::midpoint(minimum.x, maximum.x), std::midpoint(minimum.y, maximum.y), maximum.z };
}

float icon_radius(float base_radius, float distance, float max_distance, bool clustered)
{
    float const t = std::clamp(distance / max_distance, 0.0f, 1.0f);
    float const scale = (1.25f - 0.5f * t * t * (3.0f - 2.0f * t)) * (clustered ? 1.2f : 1.0f);
    return std::min(base_radius, std::numeric_limits<float>::max() / 1.5f) * std::min(scale, 1.5f);
}

bool icon_screen_tip(float px, float py, DirectX::XMFLOAT2& tip)
{
    tip = { px, py - Tip_Gap };
    return tip.y >= 0.0f;
}

bool icon_clip_tip(DirectX::XMFLOAT4 const& clip, float width, float height, DirectX::XMFLOAT2& tip)
{
    float const depth = clip.z / clip.w;
    if (depth < 0.0f || depth > 1.0f)
        return false;
    return icon_screen_tip((clip.x / clip.w * 0.5f + 0.5f) * width, (1.0f - clip.y / clip.w) * 0.5f * height, tip);
}


std::vector<IconMarker> icon_marker(std::span<IconCandidate const> candidates,
    float base_radius, float max_distance, size_t marker_limit)
{
    std::unordered_map<uint32_t, uint32_t> membership;

    std::vector<Member> members;
    members.reserve(candidates.size());
    for (IconCandidate const& candidate : candidates)
    {
        float const radius = icon_radius(base_radius, candidate.distance, max_distance, false);

        std::unordered_map<uint32_t, uint32_t>::const_iterator const previous = membership.find(candidate.form_id);
        members.emplace_back(candidate, radius, previous == membership.end() ? 0 : previous->second);
    }

    std::ranges::sort(members, [](Member const& a, Member const& b)
    {
        if ((a.previous_group == 0) != (b.previous_group == 0))
            return a.previous_group != 0;
        if (a.previous_group != b.previous_group)
            return a.previous_group < b.previous_group;
        return a.candidate.form_id < b.candidate.form_id;
    });

    std::vector<std::vector<size_t>> retained;
    size_t previous_begin = 0;
    for (size_t index = 0; index < members.size(); ++index)
    {
        if (index == 0 || members[index].previous_group == 0 || members[index].previous_group != members[index - 1].previous_group)
            previous_begin = retained.size();
        bool joined = false;
        for (size_t slot = previous_begin; slot < retained.size(); ++slot)
        {
            std::vector<size_t>& group = retained[slot];
            if (std::ranges::all_of(group, [&](size_t other) { return compatible(members[index], members[other]); }))
            {
                group.push_back(index);
                joined = true;
                break;
            }
        }
        if (!joined)
            retained.push_back({ index });
    }

    std::vector<std::vector<size_t>> groups;
    for (std::vector<size_t> const& incoming : retained)
    {
        bool joined = false;
        for (std::vector<size_t>& group : groups)
        {
            bool const fits = std::ranges::all_of(incoming, [&](size_t index)
            {
                return std::ranges::all_of(group, [&](size_t other) { return compatible(members[index], members[other]); });
            });
            if (fits)
            {
                group.insert(group.end(), incoming.begin(), incoming.end());
                joined = true;
                break;
            }
        }
        if (!joined)
            groups.push_back(incoming);
    }

    std::vector<IconMarker> markers;
    markers.reserve(groups.size());
    for (std::vector<size_t> const& group : groups)
    {
        double cx = 0.0, cy = 0.0, cz = 0.0;
        float distance = std::numeric_limits<float>::max();
        float opacity = 0.0f;
        size_t representative = members.size();
        for (size_t index : group)
        {
            Member const& member = members[index];
            cx += member.candidate.anchor.x;
            cy += member.candidate.anchor.y;
            cz += member.candidate.anchor.z;
            distance = std::min(distance, member.candidate.distance);
            opacity = std::max(opacity, std::min(member.candidate.opacity, 1.0f));
            if (member.candidate.form_id == member.previous_group &&
                (representative == members.size() || member.candidate.form_id < members[representative].candidate.form_id))
                representative = index;
        }
        if (representative == members.size())
        {
            double const count = static_cast<double>(group.size());
            DirectX::XMFLOAT3 const centroid{ static_cast<float>(cx / count), static_cast<float>(cy / count), static_cast<float>(cz / count) };
            double closest = std::numeric_limits<double>::max();
            for (size_t index : group)
            {
                double const squared = distance_squared(members[index].candidate.anchor, centroid);
                if (squared < closest || (squared == closest && members[index].candidate.form_id < members[representative].candidate.form_id))
                {
                    closest = squared;
                    representative = index;
                }
            }
        }
        IconCandidate const& target = members[representative].candidate;
        for (size_t index : group)
            membership.emplace(members[index].candidate.form_id, target.form_id);
        markers.emplace_back(target.form_id, group.size(), target.tip,
            icon_radius(base_radius, distance, max_distance, group.size() > 1), opacity);
    }
    std::unordered_map<uint32_t, float> group_distances;
    for (Member const& member : members)
    {
        uint32_t const group = membership.at(member.candidate.form_id);
        auto const [entry, inserted] = group_distances.emplace(group, member.candidate.distance);
        if (!inserted)
            entry->second = std::min(entry->second, member.candidate.distance);
    }
    std::ranges::sort(markers, [&](IconMarker const& a, IconMarker const& b)
    {
        float const da = group_distances.at(a.representative), db = group_distances.at(b.representative);
        return da != db ? da < db : a.representative < b.representative;
    });
    if (markers.size() > marker_limit)
        markers.resize(marker_limit);
    return markers;
}

ICON_NAMESPACE_END