#include "corpse_finder.h"
#include "searched_corpses.h"
#include "config/config.h"
#include "base/util.h"

PLUGIN_NAMESPACE_BEGIN

namespace
{
    // 已确认的可搜刮尸体（主线程写，渲染线程经快照读取）
    std::mutex g_mutex;
    std::vector<CorpseScan::CorpseInfo> g_corpses;


    // ---------------------------------------------------------------------------
    // 包围盒计算（参考 Precision 的碰撞体方案）
    //
    // Havok 物理空间使用"米"为单位，游戏单位 = havok 单位 × 世界尺度逆（≈70）。
    // 每个刚体的世界 AABB 直接调引擎函数 bhkRigidBody::GetAabbWorldspace()，
    // 对 box/capsule/convex/mopp 等所有形状类型都准确（即游戏真正的碰撞盒）。
    //
    // - 普通状态（含灰烬堆）：遍历 3D 树上的碰撞对象（bhkCollisionObject）；
    // - ragdoll 尸体：根碰撞体已移出 Havok 世界（变换停在死亡瞬间），改走
    //   Precision 同款路径 —— hkbRagdollDriver → hkaRagdollInstance → rigidBodies，
    //   取 ragdoll 刚体的世界 AABB；
    // - 肢解处理：两条路径的刚体 AABB 都经 largest_cluster_bounds 聚合——
    //   以体积最大的刚体为种子，只保留与其相邻的连通主体。完整尸体各部位
    //   彼此相邻（结果与全量并集一致），被肢解后飞散的零碎部位不参与包围盒；
    // - 都没有时：退化为"只取几何节点"的 worldBound 包围球累加。
    // ---------------------------------------------------------------------------

    [[nodiscard]] float hk_x(RE::hkVector4 const& a_v) { return a_v.quad.m128_f32[0]; }
    [[nodiscard]] float hk_y(RE::hkVector4 const& a_v) { return a_v.quad.m128_f32[1]; }
    [[nodiscard]] float hk_z(RE::hkVector4 const& a_v) { return a_v.quad.m128_f32[2]; }

    [[nodiscard]] RE::NiPoint3 hk_to_ni(RE::hkVector4 const& a_v)
    {
        return { hk_x(a_v), hk_y(a_v), hk_z(a_v) };
    }

    // Havok 世界尺度逆：米 → 游戏单位（引擎全局，Precision 同款地址）
    [[nodiscard]] float world_scale_inverse()
    {
        static REL::Relocation<float*> s_world_scale_inverse{ RELOCATION_ID(230692, 187407) };
        float* scale = s_world_scale_inverse.get();
        return scale ? *scale : 70.0f;
    }

    // Havok 世界变换（hkTransform，米）→ NiTransform（游戏单位）
    [[nodiscard]] RE::NiTransform hk_transform_to_ni(RE::hkTransform const& a_t)
    {
        RE::NiTransform out;
        out.scale = 1.0f;
        RE::hkRotation const& r = a_t.rotation;
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
        out.translate = { hk_x(a_t.translation) * s, hk_y(a_t.translation) * s, hk_z(a_t.translation) * s };
        return out;
    }

    void expand_aabb(RE::NiPoint3& a_min, RE::NiPoint3& a_max, RE::NiPoint3 const& a_p)
    {
        a_min.x = std::min(a_min.x, a_p.x);
        a_min.y = std::min(a_min.y, a_p.y);
        a_min.z = std::min(a_min.z, a_p.z);
        a_max.x = std::max(a_max.x, a_p.x);
        a_max.y = std::max(a_max.y, a_p.y);
        a_max.z = std::max(a_max.z, a_p.z);
    }

    // 单个 Havok 刚体的世界 AABB（GetAabbWorldspace，havok 米 → 游戏单位）
    [[nodiscard]] bool rigid_body_aabb(RE::bhkRigidBody* a_body, RE::NiPoint3& a_min, RE::NiPoint3& a_max)
    {
        if (!a_body)
            return false;

        RE::hkAabb aabb;
        a_body->GetAabbWorldspace(aabb);
        float const s = world_scale_inverse();
        a_min = { hk_x(aabb.min) * s, hk_y(aabb.min) * s, hk_z(aabb.min) * s };
        a_max = { hk_x(aabb.max) * s, hk_y(aabb.max) * s, hk_z(aabb.max) * s };
        return a_min.x <= a_max.x && a_min.y <= a_max.y && a_min.z <= a_max.z;
    }

    // 单个刚体 AABB（世界坐标，游戏单位）
    struct BodyBox
    {
        RE::NiPoint3 min;
        RE::NiPoint3 max;

        [[nodiscard]] float volume() const
        {
            RE::NiPoint3 const e = max - min;
            return e.x * e.y * e.z;
        }
    };

    // 肢解判定阈值：刚体各轴间隔不超过该值视为同一连通主体。相邻骨骼的碰撞盒
    // 彼此贴合或重叠（间隔个位数游戏单位），被肢解飞出的部位通常远离主体上百单位。
    constexpr float kClusterGap = 40.0f;

    // 以体积最大的刚体为种子，把与其邻近（各轴间隔 <= kClusterGap）的刚体迭代聚合
    // 成连通块，输出该块的 AABB 并集。完整尸体各部位相邻 → 结果与全量并集一致；
    // 被肢解（骷髅解体/部位被击飞）时只保留最大部位所在的主体，零散部位不参与包围盒。
    [[nodiscard]] bool largest_cluster_bounds(std::vector<BodyBox> const& a_boxes, RE::NiPoint3& a_min, RE::NiPoint3& a_max)
    {
        if (a_boxes.empty())
            return false;

        std::size_t seed = 0;
        for (std::size_t i = 1; i < a_boxes.size(); ++i)
        {
            if (a_boxes[i].volume() > a_boxes[seed].volume())
                seed = i;
        }

        std::vector<bool> in_cluster(a_boxes.size(), false);
        in_cluster[seed] = true;
        a_min = a_boxes[seed].min;
        a_max = a_boxes[seed].max;

        bool grew = true;
        while (grew)
        {
            grew = false;
            for (std::size_t i = 0; i < a_boxes.size(); ++i)
            {
                if (in_cluster[i])
                    continue;
                BodyBox const& box = a_boxes[i];
                bool const near_cluster =
                    box.min.x - kClusterGap <= a_max.x && box.max.x + kClusterGap >= a_min.x &&
                    box.min.y - kClusterGap <= a_max.y && box.max.y + kClusterGap >= a_min.y &&
                    box.min.z - kClusterGap <= a_max.z && box.max.z + kClusterGap >= a_min.z;
                if (!near_cluster)
                    continue;
                in_cluster[i] = true;
                expand_aabb(a_min, a_max, box.min);
                expand_aabb(a_min, a_max, box.max);
                grew = true;
            }
        }
        return true;
    }

    // 递归遍历 3D 节点树，收集"已加入 Havok 世界"的碰撞对象（普通状态/灰烬堆）：
    // - 世界 AABB：GetAabbWorldspace 逐刚体收集，随后由 largest_cluster_bounds 聚类；
    // - OBB：体积最大的盒形碰撞体（通常是 Actor 根部碰撞盒）的世界 8 角点，
    //   由形状半边长（米）与身体世界变换算出。
    void collect_collision_objects(
        RE::NiAVObject* a_node,
        std::vector<BodyBox>& a_boxes,
        RE::NiPoint3* a_obb_corners,
        bool& a_hasOBB,
        float& a_best_volume)
    {
        if (!a_node)
            return;

        if (RE::bhkCollisionObject* col_obj = a_node->GetCollisionObject())
        {
            if (RE::bhkRigidBody* body = col_obj->GetRigidBody())
            {
                if (RE::hkpRigidBody* rb = body->GetRigidBody())
                {
                    if (rb->world)
                    {
                        // 在 Havok 世界里 → 变换实时有效
                        BodyBox body_box;
                        if (rigid_body_aabb(body, body_box.min, body_box.max))
                        {
                            a_boxes.push_back(body_box);
                            if (a_obb_corners)
                            {
                                RE::hkpShape const* shape = rb->GetShape();
                                if (shape && shape->type == RE::hkpShapeType::kBox)
                                {
                                    RE::hkpBoxShape const* box = static_cast<RE::hkpBoxShape const*>(shape);
                                    float const s = world_scale_inverse();
                                    RE::NiPoint3 const he = hk_to_ni(box->halfExtents) * s;
                                    float const vol = he.x * he.y * he.z;
                                    if (vol > a_best_volume)
                                    {
                                        a_best_volume = vol;
                                        RE::NiTransform const world = hk_transform_to_ni(rb->motion.motionState.transform);
                                        for (int i = 0; i < 8; ++i)
                                        {
                                            float const sx = (i & 1) ? he.x : -he.x;
                                            float const sy = (i & 2) ? he.y : -he.y;
                                            float const sz = (i & 4) ? he.z : -he.z;
                                            a_obb_corners[i] = world * RE::NiPoint3{ sx, sy, sz };
                                        }
                                        a_hasOBB = true;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        if (RE::NiNode* node = a_node->AsNode())
        {
            for (RE::NiPointer<RE::NiAVObject> const& child : node->children)
            {
                if (child)
                    collect_collision_objects(child.get(), a_boxes, a_obb_corners, a_hasOBB, a_best_volume);
            }
        }
    }

    // ragdoll 尸体：Precision 同款 —— 收集动画图里 ragdoll 实例的所有刚体 AABB，
    // 再经 largest_cluster_bounds 取最大连通主体（肢解后只框最大部位所在主体）。
    // 这些刚体（hkaRagdollInstance::rigidBodies）就是尸体各部位的实际碰撞体。
    [[nodiscard]] bool compute_ragdoll_bounds(RE::Actor* a_actor, RE::NiPoint3& a_min, RE::NiPoint3& a_max)
    {
        RE::BSAnimationGraphManagerPtr anim_graph_manager;
        if (!a_actor->GetAnimationGraphManager(anim_graph_manager))
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

                // hkpRigidBody::userData 指向它的 bhkRigidBody 包装（Precision 同款用法）
                RE::bhkRigidBody* wrapper = reinterpret_cast<RE::bhkRigidBody*>(rb->userData);
                BodyBox body_box;
                if (rigid_body_aabb(wrapper, body_box.min, body_box.max))
                    boxes.push_back(body_box);
            }
        }
        return largest_cluster_bounds(boxes, a_min, a_max);
    }

    // 兜底：只取"几何节点"的 worldBound 包围球累加。
    // 相比旧实现（所有节点都累加，根节点的大球把盒子撑大一圈），
    // 几何节点球更贴合尸体实际轮廓。
    void expand_geometry_bounds(RE::NiAVObject* a_node, RE::NiPoint3& a_min, RE::NiPoint3& a_max)
    {
        if (!a_node)
            return;
        
        if (a_node->AsGeometry())
        {
            RE::NiBound const& bound = a_node->worldBound;
            if (bound.radius > 0.0f && bound.radius < 100000.0f)
            {
                RE::NiPoint3 const& c = bound.center;
                expand_aabb(a_min, a_max, { c.x - bound.radius, c.y - bound.radius, c.z - bound.radius });
                expand_aabb(a_min, a_max, { c.x + bound.radius, c.y + bound.radius, c.z + bound.radius });
            }
        }
        if (RE::NiNode* node = a_node->AsNode())
        {
            for (RE::NiPointer<RE::NiAVObject> const& child : node->children)
            {
                if (child)
                    expand_geometry_bounds(child.get(), a_min, a_max);
            }
        }
    }

    // 综合入口：ragdoll → ragdoll 刚体；否则 → 3D 树上的碰撞对象；最后几何兜底。
    // 返回 true 表示得到了有效的世界 AABB。
    [[nodiscard]] bool compute_bounds(
        RE::TESObjectREFR* a_ref,
        bool a_ragdoll,
        RE::NiPoint3& a_min,
        RE::NiPoint3& a_max,
        RE::NiPoint3* a_obb_corners,
        bool& a_hasOBB,
        bool& a_from_collision)
    {
        a_hasOBB = false;
        a_from_collision = false;
        if (!a_ref)
            return false;
        
        RE::NiAVObject* node = a_ref->Get3D();
        if (!node)
            return false;

        RE::NiPoint3 mn{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
        RE::NiPoint3 mx{ -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };

        if (a_ragdoll)
        {
            if (RE::Actor* actor = a_ref->As<RE::Actor>())
            {
                if (compute_ragdoll_bounds(actor, mn, mx))
                {
                    a_min = mn;
                    a_max = mx;
                    a_from_collision = true;
                    return true;
                }
            }
        } 
        else
        {
            std::vector<BodyBox> boxes;
            float best_vol = 0.0f;
            collect_collision_objects(node, boxes, a_obb_corners, a_hasOBB, best_vol);
            if (largest_cluster_bounds(boxes, mn, mx))
            {
                a_min = mn;
                a_max = mx;
                a_from_collision = true;
                return true;
            }
            a_hasOBB = false;
        }

        expand_geometry_bounds(node, mn, mx);
        if (mn.x <= mx.x && mn.y <= mx.y && mn.z <= mx.z)
        {
            a_min = mn;
            a_max = mx;
            return true;
        }
        return false;
    }

    // 静态可搜刮物体表单：localID + 插件名，运行时换算成完整 FormID。
    // 不用 EDID 前缀匹配：GetFormEditorID 是虚表调用（槽位 32），本机实测和 IsDead()
    // 一样返回垃圾值，不可用；GetFormID 是纯数据读取，安全。
    struct StaticFormID
    {
        uint32_t local;
        char const* plugin;
    };

    [[nodiscard]] std::vector<RE::FormID> resolve_form_ids(std::initializer_list<StaticFormID> a_forms)
    {
        std::vector<RE::FormID> out;
        RE::TESDataHandler* dh = RE::TESDataHandler::GetSingleton();
        if (dh)
        {
            for (auto const& [local, plugin] : a_forms)
            {
                RE::FormID const id = dh->LookupFormID(local, plugin);
                if (id != 0)
                    out.push_back(id);
            }
        }
        // 结果被 static 缓存一次：为空说明数据未就绪或主文件缺失，对应检测会整体失效
        if (out.empty())
            logger::warn("Resolved 0 of {} static form IDs, related corpse detection is disabled", a_forms.size());

        return out;
    }

    // 按 (通道, FormID) 去重的状态日志：同一 (通道, form_id) 的内容变化时才输出（首次必输出），
    // 避免每 0.5s 刷屏。三个通道（一次性跳过诊断、灰烬堆/静态尸体状态、每轮 listed 诊断）
    // 各自独立去重——只按 form_id 去重会让不同通道的同一 form_id 互相顶掉内容而反复重打。
    void log_state_once(RE::FormID a_form_id, std::string_view a_channel, std::string_view a_detail)
    {
        static std::mutex s_mutex;
        static std::vector<std::pair<RE::FormID, std::string>> s_seen;
        static std::vector<std::string> s_details;
        bool log_it = false;
        {
            std::lock_guard lock(s_mutex);
            for (std::size_t i = 0; i < s_seen.size(); ++i)
            {
                if (s_seen[i].first != a_form_id || s_seen[i].second != a_channel)
                    continue;
                if (s_details[i] == a_detail)
                    return;

                s_details[i] = std::string(a_detail);
                log_it = true;
                break;
            }
            if (!log_it)
            {
                s_seen.emplace_back(a_form_id, std::string(a_channel));
                s_details.emplace_back(a_detail);
                log_it = true;
            }
        }
        if (log_it)
            logger::info("{}", a_detail);
    }

    // 一次性诊断：被"看起来还活着"过滤器排除的 Actor（转发到 log_state_once）
    void log_once_skip(RE::Actor* a_actor, std::string_view a_reason)
    {
        log_state_once(
            a_actor->GetFormID(),
            "skip",
            fmt::format(
                "Skip non-corpse {:08X} ({}): {}",
                a_actor->GetFormID(),
                a_actor->GetDisplayFullName(),
                a_reason));
    }

    // 灰烬堆/静态尸体状态诊断（状态变化时才记日志，转发到 log_state_once）
    void log_ash_pile_state(RE::TESObjectREFR* a_ref, std::string_view a_detail)
    {
        log_state_once(
            a_ref->GetFormID(),
            "ash",
            fmt::format(
                "Ash Pile {:08X} base {:08X}: {}",
                a_ref->GetFormID(),
                a_ref->GetBaseObject() ? a_ref->GetBaseObject()->GetFormID() : 0,
                a_detail));
    }

    void filter_corpse(RE::TESObjectREFR* a_ref, Config const& a_cfg, std::vector<CorpseScan::CorpseInfo>& corpse_infos)
    {
        if (!a_ref)
            return;

        RE::TESObjectREFR* ref = Util::get_container_object(a_ref);

        if (a_cfg.hide_searched_enabled && MarkCorpse::contains(ref))
            return;

        RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton();

        CorpseScan::CorpseInfo entry;
        if (RE::Actor* actor = a_ref->As<RE::Actor>())
        {
            if (actor == player || actor->IsDisabled() || actor->IsDeleted() ||
                actor->IsReanimated() || actor->IsGhost() || !actor->Is3DLoaded() ||
                actor->IsDead())
                return;

            LootFilter::EvaluateResult const loot = LootFilter::evaluate(actor);
            if (!loot.has_items)
                return;

            RE::NiPoint3 const pos = actor->GetPosition();
            float const dist = (pos - player->GetPosition()).Length();

            entry.form_id = actor->GetFormID();
            entry.anchor = pos;
            entry.anchor.z += 40.0f;  // 默认锚点抬高到尸体中部
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
            else if (const RE::NiAVObject* node = actor->Get3D())
            {
                RE::NiBound const& bound = node->worldBound;
                if (bound.radius > 0.0f && bound.radius < 10000.0f)
                {
                    entry.anchor = bound.center;
                    entry.radius = bound.radius;
                }
            }

            corpse_infos.push_back(entry);
        }
        else
        {
            bool const is_ash = Util::is_ash_pile_ref(a_ref);
            bool const is_corpse_obj = is_ash ? false : Util::is_corpse_object_ref(a_ref);
            if (!is_ash && !is_corpse_obj)
                return;

            // only container need use owner
            LootFilter::EvaluateResult loot = LootFilter::evaluate(ref);
            if (!loot.has_items)
                return;

            entry.form_id = a_ref->GetFormID();
            entry.anchor = a_ref->GetPosition();
            entry.anchor.z += 15.0f;
            entry.radius = 40.0f;
            entry.loot_categories = loot.categories;
            entry.best_item_value = loot.best_item_value;
            entry.is_ash_pile = is_ash;
            entry.is_static_corpse = is_corpse_obj;

            if (const RE::NiAVObject* node = a_ref->Get3D())
            {
                RE::NiBound const& bound = node->worldBound;
                if (bound.radius > 0.0f && bound.radius < 10000.0f)
                {
                    entry.anchor = bound.center;
                    entry.radius = bound.radius;
                }
            }
            RE::NiPoint3 b_min, b_max;
            bool dummy_obb = false;
            bool dummy_src = false;
            if (compute_bounds(a_ref, false, b_min, b_max, nullptr, dummy_obb, dummy_src))
            {
                entry.bound_min = b_min;
                entry.bound_max = b_max;
            }
            entry.distance = (entry.anchor - player->GetPosition()).Length();

            corpse_infos.push_back(entry);
        }
    }
}


void CorpseScan::search()
{
    RE::TES* tes = RE::TES::GetSingleton();
    RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton();
    if (!tes || !player)
        return;

    Config const& cfg = Setting::get_config();

    std::vector<CorpseInfo> found;
    found.reserve(64);
    tes->ForEachReferenceInRange(player, cfg.max_distance, [&](RE::TESObjectREFR* a_ref) -> RE::BSContainer::ForEachResult
    {
        filter_corpse(a_ref, cfg, found);
        return RE::BSContainer::ForEachResult::kContinue;
    });

    // 取回旧表在本线程（游戏线程）析构，避免渲染线程在快照交换瞬间
    // 与扫描线程并发触碰同一批 CorpseEntry
    std::vector<CorpseInfo> previous;
    {
        std::lock_guard lock(g_mutex);
        previous = std::move(g_corpses);
        g_corpses = std::move(found);
    }
    previous.clear();
}

std::vector<CorpseScan::CorpseInfo> CorpseScan::snapshot()
{
    std::lock_guard lock(g_mutex);
    return g_corpses;
}

PLUGIN_NAMESPACE_END