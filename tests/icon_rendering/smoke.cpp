//
// Created by AmazingBuff on 2026/09/17.
//

#include "render/icon/icon_geometry.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace Amazing::HighlightLootableCorpses
{
void check(bool condition, char const* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

IconCandidate candidate(std::uint32_t id, float world_x = 0.0f, float screen_x = 100.0f)
{
    return { id, { world_x, 0, 0 }, { screen_x, 100 }, 500, 0.5f };
}

void test_layout()
{
    IconLayout layout;
    auto const update = [&](std::vector<IconCandidate> const& values, float width = 800.0f)
    {
        return layout.update(values, 10, 2000, width, 600, 16);
    };
    std::vector<IconCandidate> values{ candidate(1), candidate(2, 100, 110) };
    std::vector<IconMarker> markers = update(values);
    check(markers.size() == 1 && markers[0].member_count == 2 && markers[0].representative == 1, "join and centroid tie");
    values[1].anchor.x = 210;
    values[1].tip.x = 134;
    check(update(values).size() == 1, "world/screen exit hysteresis");
    std::ranges::reverse(values);
    check(update(values)[0].representative == 1, "input shuffle retains representative");
    values[0].anchor.x = 230;
    check(update(values).size() == 2, "exit bound splits");
    values[0].anchor.x = 200;
    check(update(values).size() == 2, "split pair must satisfy join threshold");
    values[0].anchor.x = 100;
    values[0].tip.x = 110;
    check(update(values).size() == 1, "rejoin");
    check(update({}).empty(), "empty clears output");
    values[0].anchor.x = 200;
    check(update(values).size() == 2, "empty clears membership");
    layout.reset();
    values = { candidate(10, 0), candidate(20, 100), candidate(30, 200) };
    markers = update(values);
    check(markers.size() == 2 && markers[0].member_count == 2, "complete-link rejects chain");
    layout.reset();
    values = { candidate(10, 0), candidate(20, 100) };
    check(update(values).size() == 1, "existing pair");
    values.push_back(candidate(1, -100));
    markers = update(values);
    check(markers.size() == 2 && markers[1].representative == 10 && markers[1].member_count == 2, "arriving low ID does not steal pair");
    layout.reset();
    values = { candidate(1), candidate(2, 20) };
    values[1].anchor.z = 65;
    check(update(values).size() == 2, "different floors separated");
    values[1].anchor.z = 0;
    values[1].anchor.x = 181;
    check(update(values).size() == 2, "distant aligned targets separated");
    values[1].anchor.x = 20;
    values[1].tip.x = 200;
    check(update(values).size() == 2, "screen separation");
    values[1].tip.x = 110;
    check(update(values).size() == 1, "screen approach merges");
    values[1].anchor.z = 75;
    check(update(values).size() == 1, "height hysteresis");
    check(update(values, 801).size() == 2, "viewport reset");
    layout.reset();
    values = { candidate(1, 0), candidate(2, 50), candidate(3, 100) };
    check(update(values)[0].representative == 2, "representative nearest centroid");
    values.push_back(candidate(4, 170));
    values[0].opacity = 0.8f;
    values[0].distance = 0;
    markers = update(values);
    check(markers[0].representative == 2 && markers[0].radius == 15 && markers[0].opacity == 0.8f, "representative retained and group prominence");
    std::erase_if(values, [](IconCandidate const& value) { return value.form_id == 2; });
    check(update(values)[0].representative == 3, "removed representative replaced by actual member");
    layout.reset();
    values.clear();
    for (std::uint32_t id = 1; id <= 40; ++id)
        values.push_back(candidate(id, static_cast<float>(id), 100));
    markers = update(values);
    check(markers.size() == 1 && markers[0].member_count == 40, "cluster all candidates before output budget");
    layout.reset();
    values.clear();
    for (std::uint32_t id = 1; id <= 20; ++id)
    {
        IconCandidate value = candidate(id, static_cast<float>(id) * 500);
        value.distance = static_cast<float>(21 - id) * 10;
        values.push_back(value);
    }
    markers = update(values);
    check(markers.size() == 16 && markers.front().representative == 20 && markers.back().representative == 5, "nearest16 output priority");
    values.push_back(candidate(21, 510));
    values.back().distance = 201;
    (void)update(values);
    values.back().anchor.x = 710;
    values[0].distance = 0;
    markers = update(values);
    check(markers[0].member_count == 2, "hidden groups retain membership history");
    layout.reset();
    values = { candidate(1), candidate(2), candidate(3), candidate(4), candidate(5) };
    values[0].opacity = 0;
    values[1].anchor.x = std::numeric_limits<float>::quiet_NaN();
    values[2].tip.x = -1;
    values[3].distance = -1;
    values[4].tip.y = 600;
    check(update(values).empty(), "invalid and invisible candidates omitted");
    std::cout << "PASS: grouping, hysteresis, representatives, visibility and output budget\n";
}

void test_geometry()
{
    float const nan = std::numeric_limits<float>::quiet_NaN();
    check(icon_radius(10, 0, 2000, false) == 12.5f, "near scale");
    check(icon_radius(10, 1000, 2000, false) == 10, "smoothstep midpoint");
    check(icon_radius(10, 3000, 2000, false) == 7.5f, "far clamp");
    check(icon_radius(10, 0, 2000, true) == 15, "cluster clamp");
    check(icon_radius(10, nan, 2000, false) == 0 && icon_radius(10, 0, 0, false) == 0, "invalid size input");
    DirectX::XMFLOAT3 const fallback{ 4, 5, 6 };
    DirectX::XMFLOAT3 const top = icon_anchor(fallback, { -2, -4, -8 }, { 4, 8, 12 });
    check(top.x == 1 && top.y == 2 && top.z == 12, "AABB top center");
    check(icon_anchor(fallback, {}, {}).z == 6 && icon_anchor(fallback, { nan, 0, 0 }, { 1, 1, 1 }).z == 6 &&
        icon_anchor(fallback, { 2, 0, 0 }, { 1, 1, 1 }).z == 6, "invalid AABB fallback");
    DirectX::XMFLOAT2 tip;
    check(icon_screen_tip(100, 100, 1, 800, 600, tip) && tip.y == 94, "six pixel gap");
    check(!icon_screen_tip(100, 100, -1, 800, 600, tip) && !icon_screen_tip(900, 100, 1, 800, 600, tip) &&
        !icon_screen_tip(nan, 100, 1, 800, 600, tip) && !icon_screen_tip(100, 2, 1, 800, 600, tip), "invalid/offscreen/behind projection");
    check(icon_clip_tip({ 0, 0, 0.5f, 1 }, 800, 600, tip) && tip.x == 400 && tip.y == 294,
        "clip fallback projection");
    check(!icon_clip_tip({ 0, 0, -0.5f, -1 }, 800, 600, tip) &&
        !icon_clip_tip({ 0, 0, 0, 0 }, 800, 600, tip) &&
        !icon_clip_tip({ 0, 0, 2, 1 }, 800, 600, tip), "clip fallback rejects behind/invalid depth");
    for (float base : { 5.0f, 10.0f, 20.0f })
    {
        for (bool cluster : { false, true })
        {
            IconMarker const marker{ 1, cluster ? 2u : 1u, { 100, 100 }, icon_radius(base, 2000, 2000, cluster), 0.25f };
            IconGeometry const geometry = icon_geometry(marker, { 0, 1, 0 }, 800, 600);
            check(geometry.count == (cluster ? 42u : 21u), "distinct single/double geometry");
            float bottom = -1000;
            double area = 0;
            for (std::size_t index = 0; index < geometry.count; index += 3)
            {
                DirectX::XMFLOAT3 const a = geometry.vertices[index].pos, b = geometry.vertices[index + 1].pos, c = geometry.vertices[index + 2].pos;
                area += std::abs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) * 0.5 * 400 * 300;
                for (std::size_t j = index; j < index + 3; ++j)
                {
                    IconVertex const& vertex = geometry.vertices[j];
                    bottom = std::max(bottom, (1.0f - vertex.pos.y) * 300);
                    check(vertex.color.w == 0.25f, "no double alpha multiplication");
                }
            }
            check(std::abs(bottom - 100) < 0.001f, "fixed bottom tip");
            double const expected = marker.radius * marker.radius * 1.5 * (cluster ? 2 : 1);
            check(std::abs(area - expected) < 0.02, "ring plus fill area partitions outer triangle");
        }
    }
    IconMarker marker{ 1, 2, { 100, 100 }, 10, 0 };
    check(icon_geometry(marker, { 0, 1, 0 }, 800, 600).count == 0, "zero opacity emits nothing");
    marker.opacity = nan;
    check(icon_geometry(marker, { 0, 1, 0 }, 800, 600).count == 0, "nonfinite alpha omitted");
    check(16 * Icon_Marker_Vertex_Count == 672, "complete double marker budget");
    std::cout << "PASS: size curve, anchors, geometry, alpha and capacity\n";
}

void test_gpu(std::filesystem::path const& root);
}

int main(int argc, char** argv)
{
    try
    {
        if (argc != 2)
            throw std::runtime_error("expected repository root");
        Amazing::HighlightLootableCorpses::test_layout();
        Amazing::HighlightLootableCorpses::test_geometry();
        Amazing::HighlightLootableCorpses::test_gpu(argv[1]);
        return 0;
    }
    catch (std::exception const& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
