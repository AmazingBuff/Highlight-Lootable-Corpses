#include "corpse_finder.h"

#include "searched_corpses.h"

#include "base/util.h"
#include "config/config.h"

PLUGIN_NAMESPACE_BEGIN

namespace
{
    // ---------------------------------------------------------------------------
    // Bounding box computation (follows Precision's collision-body approach)
    //
    // Havok physics space is measured in metres: game units = havok units × world scale inverse (≈70).
    // A rigid body's world AABB comes straight from the engine function
    // bhkRigidBody::GetAabbWorldspace(), which is accurate for all shape types - box/capsule/
    // convex/mopp and so on (i.e. the game's real collision boxes).
    //
    // - Ordinary state (ash piles included): walk the collision objects on the 3D tree
    //   (bhkCollisionObject);
    // - ragdoll corpse: the root collision body has been moved out of the Havok world (its
    //   transform froze at the moment of death), so take Precision's path instead -
    //   hkbRagdollDriver → hkaRagdollInstance → rigidBodies - and read the ragdoll rigid bodies'
    //   world AABBs;
    // - dismemberment handling: the rigid-body AABBs from both paths are aggregated by
    //   largest_cluster_bounds - the largest body by volume is the seed and only the connected mass
    //   adjacent to it is kept. On an intact corpse every part is adjacent to the next (the result
    //   equals the full union), while the scattered parts flying off after dismemberment do not
    //   enter the bounding box;
    // - when neither yields anything: degrade to accumulating the worldBound bounding spheres of
    //   the geometry nodes only.
    // ---------------------------------------------------------------------------

    [[nodiscard]] float hk_x(RE::hkVector4 const& v) { return v.quad.m128_f32[0]; }
    [[nodiscard]] float hk_y(RE::hkVector4 const& v) { return v.quad.m128_f32[1]; }
    [[nodiscard]] float hk_z(RE::hkVector4 const& v) { return v.quad.m128_f32[2]; }

    [[nodiscard]] RE::NiPoint3 hk_to_ni(RE::hkVector4 const& v)
    {
        return { hk_x(v), hk_y(v), hk_z(v) };
    }

    // Havok world scale inverse: metres → game units (engine global, the same address as Precision)
    [[nodiscard]] float world_scale_inverse()
    {
        static REL::Relocation<float*> s_world_scale_inverse{ RELOCATION_ID(230692, 187407) };
        float* scale = s_world_scale_inverse.get();
        return scale ? *scale : 70.0f;
    }

    // Havok world transform (hkTransform, metres) → NiTransform (game units)
    [[nodiscard]] RE::NiTransform hk_transform_to_ni(RE::hkTransform const& t)
    {
        RE::NiTransform out;
        out.scale = 1.0f;
        RE::hkRotation const& r = t.rotation;
        out.rotate.entry[0][0] = hk_x(r.col0);
        out.rotate.entry[0][1] = hk_x(r.col1);
        out.rotate.entry[0][2] = hk_x(r.col2);
        out.rotate.entry[1][0] = hk_y(r.col0);
        out.rotate.entry[1][1] = hk_y(r.col1);
        out.rotate.entry[1][2] = hk_y(r.col2);
        out.rotate.entry[2][0] = hk_z(r.col0);
        out.rotate.entry[2][1] = hk_z(r.col1);
        out.rotate.entry[2][2] = hk_z(r.col2);
        float const s = world_scale_inverse();
        out.translate = { hk_x(t.translation) * s, hk_y(t.translation) * s, hk_z(t.translation) * s };
        return out;
    }

    void expand_aabb(RE::NiPoint3& min, RE::NiPoint3& max, RE::NiPoint3 const& p)
    {
        min.x = std::min(min.x, p.x);
        min.y = std::min(min.y, p.y);
        min.z = std::min(min.z, p.z);
        max.x = std::max(max.x, p.x);
        max.y = std::max(max.y, p.y);
        max.z = std::max(max.z, p.z);
    }

    // World AABB of a single Havok rigid body (GetAabbWorldspace, havok metres → game units)
    [[nodiscard]] bool rigid_body_aabb(RE::bhkRigidBody* body, RE::NiPoint3& min, RE::NiPoint3& max)
    {
        if (!body)
            return false;

        RE::hkAabb aabb;
        body->GetAabbWorldspace(aabb);
        float const s = world_scale_inverse();
        min = { hk_x(aabb.min) * s, hk_y(aabb.min) * s, hk_z(aabb.min) * s };
        max = { hk_x(aabb.max) * s, hk_y(aabb.max) * s, hk_z(aabb.max) * s };
        return min.x <= max.x && min.y <= max.y && min.z <= max.z;
    }

    // AABB of a single rigid body (world coordinates, game units)
    struct BodyBox
    {
        RE::NiPoint3 min;
        RE::NiPoint3 max;
    };

    [[nodiscard]] float body_box_volume(BodyBox const& box)
    {
        RE::NiPoint3 const e = box.max - box.min;
        return e.x * e.y * e.z;
    }

    // Dismemberment threshold: rigid bodies whose per-axis gap does not exceed this value count as
    // one connected mass. Adjacent bones' collision boxes touch or overlap (a single-digit gap in
    // game units), while parts blasted off by dismemberment usually sit hundreds of units away.
    constexpr float Cluster_Gap = 40.0f;

    // Seed with the largest rigid body by volume and iteratively aggregate the bodies adjacent to
    // it (per-axis gap <= Cluster_Gap) into one connected component, then output that component's
    // AABB union. On an intact corpse every part is adjacent → the result equals the full union;
    // after dismemberment (a skeleton falling apart / a part being blasted off) only the mass that
    // holds the largest part is kept and the scattered parts do not enter the bounding box.
    [[nodiscard]] bool largest_cluster_bounds(std::vector<BodyBox> const& boxes, RE::NiPoint3& min, RE::NiPoint3& max)
    {
        if (boxes.empty())
            return false;

        size_t seed = 0;
        for (size_t i = 1; i < boxes.size(); ++i)
        {
            if (body_box_volume(boxes[i]) > body_box_volume(boxes[seed]))
                seed = i;
        }

        std::vector<uint8_t> in_cluster(boxes.size(), 0);
        in_cluster[seed] = true;
        min = boxes[seed].min;
        max = boxes[seed].max;

        bool grew = true;
        while (grew)
        {
            grew = false;
            for (size_t i = 0; i < boxes.size(); ++i)
            {
                if (in_cluster[i])
                    continue;
                BodyBox const& box = boxes[i];
                bool const near_cluster =
                    box.min.x - Cluster_Gap <= max.x && box.max.x + Cluster_Gap >= min.x &&
                    box.min.y - Cluster_Gap <= max.y && box.max.y + Cluster_Gap >= min.y &&
                    box.min.z - Cluster_Gap <= max.z && box.max.z + Cluster_Gap >= min.z;
                if (!near_cluster)
                    continue;
                in_cluster[i] = 1;
                expand_aabb(min, max, box.min);
                expand_aabb(min, max, box.max);
                grew = true;
            }
        }
        return true;
    }

    // Recursively walk the 3D node tree and collect the collision objects that are "in the Havok
    // world" (ordinary state / ash piles):
    // - world AABB: collected per rigid body with GetAabbWorldspace, then clustered by
    //   largest_cluster_bounds;
    // - OBB: the world 8 corners of the largest box-shaped collision body by volume (usually the
    //   Actor's root collision box), computed from the shape half extents (metres) and the body's
    //   world transform.
    void collect_collision_objects(RE::NiAVObject* object, std::vector<BodyBox>& boxes, RE::NiPoint3* obb_corners, bool& has_obb, float& best_volume)
    {
        if (!object)
            return;

        if (RE::bhkCollisionObject* col_obj = object->GetCollisionObject())
        {
            if (RE::bhkRigidBody* body = col_obj->GetRigidBody())
            {
                if (RE::hkpRigidBody* rb = body->GetRigidBody())
                {
                    if (rb->world)
                    {
                        // in the Havok world → the transform is live
                        BodyBox body_box;
                        if (rigid_body_aabb(body, body_box.min, body_box.max)) 
                        {
                            boxes.push_back(body_box);
                            
                            RE::hkpShape const* shape = rb->GetShape();
                            if (shape && shape->type == RE::hkpShapeType::kBox)
                            {
                                RE::hkpBoxShape const* box = static_cast<RE::hkpBoxShape const*>(shape);
                                float const s = world_scale_inverse();
                                RE::NiPoint3 const he = hk_to_ni(box->halfExtents) * s;
                                float const vol = he.x * he.y * he.z;
                                if (vol > best_volume)
                                {
                                    best_volume = vol;
                                    RE::NiTransform const world = hk_transform_to_ni(rb->motion.motionState.transform);
                                    for (int i = 0; i < 8; ++i)
                                    {
                                        float const sx = (i & 1) ? he.x : -he.x;
                                        float const sy = (i & 2) ? he.y : -he.y;
                                        float const sz = (i & 4) ? he.z : -he.z;
                                        obb_corners[i] = world * RE::NiPoint3{ sx, sy, sz };
                                    }
                                    has_obb = true;
                                }
                            }
                        }
                    }
                }
            }
        }
        
        if (RE::NiNode* node = object->AsNode())
        {
            for (RE::NiPointer<RE::NiAVObject> const& child : node->children)
            {
                if (child)
                    collect_collision_objects(child.get(), boxes, obb_corners, has_obb, best_volume);
            }
        }
    }

    // ragdoll corpse: the same as Precision - collect the AABBs of all rigid bodies of the ragdoll
    // instance in the animation graph, then take the largest connected mass through
    // largest_cluster_bounds (after dismemberment only the mass holding the largest part is
    // framed). These rigid bodies (hkaRagdollInstance::rigidBodies) are the corpse parts' actual
    // collision bodies.
    [[nodiscard]] bool compute_ragdoll_bounds(RE::Actor* actor, RE::NiPoint3& min, RE::NiPoint3& max)
    {
        RE::BSAnimationGraphManagerPtr anim_graph_manager;
        if (!actor->GetAnimationGraphManager(anim_graph_manager))
            return false;

        std::vector<BodyBox> boxes;
        RE::BSSpinLockGuard lock(anim_graph_manager->GetRuntimeData().updateLock);
        for (RE::BSTSmartPointer<RE::BShkbAnimationGraph> const& graph : anim_graph_manager->graphs)
        {
            if (!graph)
                continue;

            RE::hkRefPtr<RE::hkbRagdollDriver> const& driver = graph->characterInstance.ragdollDriver;
            if (!driver)
                continue;

            RE::hkaRagdollInstance* ragdoll = driver->ragdoll;
            if (!ragdoll)
                continue;

            for (RE::hkpRigidBody const* rb : ragdoll->rigidBodies)
            {
                if (!rb)
                    continue;

                // hkpRigidBody::userData points at its bhkRigidBody wrapper (the same use as Precision)
                RE::bhkRigidBody* wrapper = reinterpret_cast<RE::bhkRigidBody*>(rb->userData);
                BodyBox body_box;
                if (rigid_body_aabb(wrapper, body_box.min, body_box.max))
                    boxes.push_back(body_box);
            }
        }
        return largest_cluster_bounds(boxes, min, max);
    }

    // Fallback: accumulate only the worldBound bounding spheres of the geometry nodes.
    // Compared with the old implementation (which accumulated every node, so the root node's large
    // sphere inflated the box by a margin), the geometry-node spheres hug the corpse's actual
    // outline more closely.
    void expand_geometry_bounds(RE::NiAVObject* object, RE::NiPoint3& min, RE::NiPoint3& max)
    {
        if (!object)
            return;

        if (object->AsGeometry())
        {
            RE::NiBound const& bound = object->worldBound;
            if (bound.radius > 0.0f && bound.radius < 100000.0f)
            {
                RE::NiPoint3 const& c = bound.center;
                expand_aabb(min, max, { c.x - bound.radius, c.y - bound.radius, c.z - bound.radius });
                expand_aabb(min, max, { c.x + bound.radius, c.y + bound.radius, c.z + bound.radius });
            }
        }
        if (RE::NiNode* node = object->AsNode())
        {
            for (RE::NiPointer<RE::NiAVObject> const& child : node->children)
            {
                if (child)
                    expand_geometry_bounds(child.get(), min, max);
            }
        }
    }

    // Combined entry point: ragdoll → the ragdoll rigid bodies; otherwise → the collision objects
    // on the 3D tree; and finally the geometry fallback. Returning true means a valid world AABB
    // was obtained.
    [[nodiscard]] bool compute_bounds(RE::TESObjectREFR* ref, bool ragdoll, RE::NiPoint3& min, RE::NiPoint3& max, RE::NiPoint3* obb_corners, bool& has_obb, bool& from_collision)
    {
        has_obb = false;
        from_collision = false;
        if (!ref)
            return false;

        RE::NiAVObject* node = ref->Get3D();
        if (!node)
            return false;

        RE::NiPoint3 mn{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
        RE::NiPoint3 mx{ -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };

        if (ragdoll)
        {
            if (RE::Actor* actor = ref->As<RE::Actor>())
            {
                if (compute_ragdoll_bounds(actor, mn, mx))
                {
                    min = mn;
                    max = mx;
                    from_collision = true;
                    return true;
                }
            }
        }
        else
        {
            std::vector<BodyBox> boxes;
            float best_vol = 0.0f;
            collect_collision_objects(node, boxes, obb_corners, has_obb, best_vol);
            if (largest_cluster_bounds(boxes, mn, mx))
            {
                min = mn;
                max = mx;
                from_collision = true;
                return true;
            }
            has_obb = false;
        }

        expand_geometry_bounds(node, mn, mx);
        if (mn.x <= mx.x && mn.y <= mx.y && mn.z <= mx.z)
        {
            min = mn;
            max = mx;
            return true;
        }
        return false;
    }

    bool filter_corpse(RE::TESObjectREFR* ref, CorpseScan::CorpseInfo& corpse_info)
    {
        if (!ref)
            return false;

        RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton();

        CorpseScan::CorpseInfo entry{ .radius = 60.0f };
        if (RE::Actor* actor = ref->As<RE::Actor>())
        {
            if (actor == player || actor->IsDisabled() || actor->IsDeleted() ||
                actor->IsReanimated() || actor->IsGhost() || !actor->Is3DLoaded() ||
                !Util::is_corpse_actor(actor))
                return false;

            LootFilter::EvaluateResult const loot = LootFilter::instance().evaluate(actor);
            if (!loot.has_items)
                return false;

            RE::NiPoint3 const pos = actor->GetPosition();
            float const dist = (pos - player->GetPosition()).Length();

            entry.form_id = actor->GetFormID();
            entry.anchor = pos;
            entry.anchor.z += 40.0f;  // raise the default anchor to the middle of the corpse
            entry.radius = 60.0f;
            entry.distance = dist;
            entry.loot_categories = loot.categories;
            entry.best_item_value = loot.best_item_value;

            RE::NiPoint3 b_min, b_max;
            if (compute_bounds(actor, actor->IsInRagdollState(), b_min, b_max, entry.obb_corners, entry.has_obb, entry.bounds_from_collision))
            {
                entry.bound_min = b_min;
                entry.bound_max = b_max;
                entry.anchor = { (b_min.x + b_max.x) * 0.5f, (b_min.y + b_max.y) * 0.5f, (b_min.z + b_max.z) * 0.5f };
                entry.radius = std::max((b_max - b_min).Length() * 0.5f, 10.0f);
            }
            else if (RE::NiAVObject const* node = actor->Get3D())
            {
                RE::NiBound const& bound = node->worldBound;
                if (bound.radius > 0.0f && bound.radius < 10000.0f)
                {
                    entry.anchor = bound.center;
                    entry.radius = bound.radius;
                }
            }
        }
        else
        {
            bool const is_ash = Util::is_ash_pile(ref);
            bool const is_corpse_obj = Util::is_corpse_object(ref);
            if (!is_ash && !is_corpse_obj)
                return false;

            // only container need use owner
            LootFilter::EvaluateResult loot = LootFilter::instance().evaluate(Util::get_container_object(ref));
            if (!loot.has_items)
                return false;

            entry.form_id = ref->GetFormID();
            entry.anchor = ref->GetPosition();
            entry.anchor.z += 15.0f;
            entry.radius = 40.0f;
            entry.loot_categories = loot.categories;
            entry.best_item_value = loot.best_item_value;
            entry.is_ash_pile = is_ash;
            entry.is_static_corpse = is_corpse_obj;

            if (RE::NiAVObject const* object = ref->Get3D())
            {
                RE::NiBound const& bound = object->worldBound;
                if (bound.radius > 0.0f && bound.radius < 10000.0f)
                {
                    entry.anchor = bound.center;
                    entry.radius = bound.radius;
                }
            }
            RE::NiPoint3 b_min, b_max;
            bool dummy_obb = false;
            bool dummy_src = false;
            if (compute_bounds(ref, false, b_min, b_max, nullptr, dummy_obb, dummy_src))
            {
                entry.bound_min = b_min;
                entry.bound_max = b_max;
            }
            entry.distance = (entry.anchor - player->GetPosition()).Length();
        }

        corpse_info = entry;

        return true;
    }
}

CorpseScan& CorpseScan::instance()
{
    static CorpseScan s_instance;
    return s_instance;
}

void CorpseScan::search()
{
    RE::TES* tes = RE::TES::GetSingleton();
    RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton();
    if (!tes || !player)
        return;

    Config const& cfg = Setting::instance().get_config();

    std::vector<CorpseInfo> found;
    found.reserve(64);
    tes->ForEachReferenceInRange(player, cfg.max_distance, [&](RE::TESObjectREFR* obj_ref) -> RE::BSContainer::ForEachResult
    {
        RE::TESObjectREFR* ref = Util::get_container_object(obj_ref);

        if (cfg.hide_searched_enabled && MarkCorpse::instance().contains(ref))
            return RE::BSContainer::ForEachResult::kContinue;

        if (CorpseInfo info{ .radius = 60.0f }; filter_corpse(obj_ref, info))
        {
            found.push_back(info);

            RE::FormID const form_id = ref->GetFormID();
            if (!m_logged_corpses.contains(form_id))
                logger::info("{} ({:08x}) has been added!", ref->GetDisplayFullName(), form_id);
            m_logged_corpses.insert(form_id);
        }
        return RE::BSContainer::ForEachResult::kContinue;
    });

    // sort by distance
    std::ranges::sort(found, [](CorpseInfo const& l, CorpseInfo const& r)
    {
        if (l.distance < r.distance)
            return true;
        return false;
    });

    {
        std::lock_guard lock(m_mutex);
        m_corpses.swap(found);
    }
}

std::vector<CorpseScan::CorpseInfo> CorpseScan::snapshot()
{
    std::lock_guard lock(m_mutex);
    return m_corpses;
}

CorpseScan::CorpseScan() = default;

CorpseScan::~CorpseScan() = default;

PLUGIN_NAMESPACE_END