#pragma once

#include "filter/loot_filter.h"

#include "Plugin.h"

#include <RE/Skyrim.h>

#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <vector>

PLUGIN_NAMESPACE_BEGIN

class CorpseScan
{
public:
    CorpseScan(CorpseScan const&) = delete;
    CorpseScan(CorpseScan const&&) = delete;
    CorpseScan operator=(CorpseScan&) = delete;
    CorpseScan operator=(CorpseScan&&) = delete;

    static CorpseScan& instance();

    struct CorpseInfo
    {
        RE::FormID form_id;              // FormID of the Actor / ash pile
        RE::NiPoint3 anchor;             // world-coordinate anchor (the middle of the corpse/ash pile, used for projection)
        RE::NiPoint3 bound_min;          // minimum corner of the 3D world bounding box (AABB)
        RE::NiPoint3 bound_max;          // maximum corner of the 3D world bounding box (AABB)
        float radius;                    // world bounding-sphere radius (fallback use)
        float distance;                  // distance to the player
        bool is_ash_pile;                // whether this is an ash pile (what a reanimated corpse turns into when it dies again)
        bool is_static_corpse;           // whether this is a static corpse (container objects such as mummified/wrapped/burnt corpses)

        // The 8 world-space corners of the collision box (OBB). With has_obb=true the renderer draws
        // a 3D wireframe box matching the collision box; otherwise it degrades to an AABB screen
        // rectangle. Corner order: bit0=x(large), bit1=y(large), bit2=z(large).
        bool has_obb;
        RE::NiPoint3 obb_corners[8];
        bool bounds_from_collision;  // diagnostics: whether the bounds came from Havok collision bodies (otherwise the geometry fallback)

        // Loot filtering (computed during the scan by LootFilter::evaluate): a bit mask of the matched value categories and the highest single-item value
        RE::stl::enumeration<LootFilter::Category> loot_categories;
        int32_t best_item_value;
    };

    void search();
    [[nodiscard]] std::vector<CorpseInfo> snapshot();
private:
    CorpseScan();
    ~CorpseScan();
private:
    // Confirmed lootable corpses (written by the main thread, read by the render thread through a snapshot)
    std::mutex m_mutex;
    std::vector<CorpseInfo> m_corpses;
    std::unordered_set<RE::FormID> m_logged_corpses;
};

PLUGIN_NAMESPACE_END
