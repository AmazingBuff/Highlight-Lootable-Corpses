#include "pch.h"
#include "corpse_finder.h"
#include "config.h"

namespace
{
    constexpr char const* SkyrimPlugin = "Skyrim.esm";
    constexpr char const* DawnguardPlugin = "Dawnguard.esm";
    constexpr char const* DragonbornPlugin = "Dragonborn.esm";

    // 已确认的可搜刮尸体（主线程写，渲染线程经快照读取）
    std::mutex g_mutex;
    std::vector<CorpseFinder::CorpseEntry> g_corpses;

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
    //   取所有 ragdoll 刚体的世界 AABB 并集，这正是躺尸的实际碰撞范围；
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
        static REL::Relocation<float*> g_world_scale_inverse{ RELOCATION_ID(230692, 187407) };
        float* scale = g_world_scale_inverse.get();
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
    [[nodiscard]] bool add_rigid_body_aabb(RE::bhkRigidBody* a_body, RE::NiPoint3& a_min, RE::NiPoint3& a_max)
    {
        if (!a_body)
            return false;
        
        RE::hkAabb aabb;
        a_body->GetAabbWorldspace(aabb);
        float const s = world_scale_inverse();
        RE::NiPoint3 const mn{ hk_x(aabb.min) * s, hk_y(aabb.min) * s, hk_z(aabb.min) * s };
        RE::NiPoint3 const mx{ hk_x(aabb.max) * s, hk_y(aabb.max) * s, hk_z(aabb.max) * s };
        if (mn.x > mx.x || mn.y > mx.y || mn.z > mx.z)
            return false;
        
        expand_aabb(a_min, a_max, mn);
        expand_aabb(a_min, a_max, mx);
        return true;
    }

    // 递归遍历 3D 节点树，收集"已加入 Havok 世界"的碰撞对象（普通状态/灰烬堆）：
    // - 世界 AABB：GetAabbWorldspace 并集；
    // - OBB：体积最大的盒形碰撞体（通常是 Actor 根部碰撞盒）的世界 8 角点，
    //   由形状半边长（米）与身体世界变换算出。
    void expand_collision_objects(
        RE::NiAVObject* a_node,
        RE::NiPoint3& a_min,
        RE::NiPoint3& a_max,
        std::size_t& a_count,
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
                        if (add_rigid_body_aabb(body, a_min, a_max))
                        {
                            ++a_count;
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
                    expand_collision_objects(child.get(), a_min, a_max, a_count, a_obb_corners, a_hasOBB, a_best_volume);
            }
        }
    }

    // ragdoll 尸体：Precision 同款 —— 取动画图里 ragdoll 实例的所有刚体 AABB 并集。
    // 这些刚体（hkaRagdollInstance::rigidBodies）就是尸体各部位的实际碰撞体，
    // 与躺姿完全一致。
    [[nodiscard]] bool compute_ragdoll_bounds(RE::Actor* a_actor, RE::NiPoint3& a_min, RE::NiPoint3& a_max)
    {
        RE::BSAnimationGraphManagerPtr anim_graph_manager;
        if (!a_actor->GetAnimationGraphManager(anim_graph_manager))
            return false;

        std::size_t count = 0;
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
                if (add_rigid_body_aabb(wrapper, a_min, a_max))
                    ++count;
            }
        }
        return count > 0;
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
                mn = RE::NiPoint3{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
                mx = RE::NiPoint3{ -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };
            }
        } 
        else
        {
            std::size_t count = 0;
            float best_vol = 0.0f;
            expand_collision_objects(node, mn, mx, count, a_obb_corners, a_hasOBB, best_vol);
            if (count > 0 && mn.x <= mx.x && mn.y <= mx.y && mn.z <= mx.z)
            {
                a_min = mn;
                a_max = mx;
                a_from_collision = true;
                return true;
            }
            mn = RE::NiPoint3{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
            mx = RE::NiPoint3{ -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };
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
        return out;
    }

    // 灰烬堆激活体（ESM 解析确认）：
    // Skyrim.esm：DefaultAshPile1 = 0x1B, DefaultAshPile2 = 0x22,
    // 幽灵/冰霜变体：Ghost = 0x101048, Ice = 0x1069E4, DarkGhost = 0x10C649, GhostBlack = 0x10D6EF
    // Dawnguard.esm：DLC01DefaultAshPileSoul（灵魂石冢 Soul Ember）= 0xA905,
    // DLC01DefaultAshPileEnemies（魂冢敌人）= 0xBDCE, DLC1dunHarkonAshPile = 0xFC74,
    // DLC1_WESC08AshPile = 0x3522
    // Dragonborn.esm：DLC2AshSpawnAshPile（灰烬魔）= 0x3280A, DLC2HMDaedraAshPile = 0x23F83
    [[nodiscard]] std::vector<RE::FormID> const& ash_pile_form_ids()
    {
        static std::vector<RE::FormID> const Ids = resolve_form_ids({
            { .local = 0x0000001B, .plugin = SkyrimPlugin }, 
            { .local = 0x00000022, .plugin = SkyrimPlugin },
            { .local = 0x00101048, .plugin = SkyrimPlugin }, 
            { .local = 0x001069E4, .plugin = SkyrimPlugin },
            { .local = 0x0010C649, .plugin = SkyrimPlugin }, 
            { .local = 0x0010D6EF, .plugin = SkyrimPlugin },
            { .local = 0x0000A905, .plugin = DawnguardPlugin },   // DLC01DefaultAshPileSoul (Soul Ember)
            { .local = 0x0000BDCE, .plugin = DawnguardPlugin },   // DLC01DefaultAshPileEnemies
            { .local = 0x0000FC74, .plugin = DawnguardPlugin },   // DLC1dunHarkonAshPile
            { .local = 0x00003522, .plugin = DawnguardPlugin },   // DLC1_WESC08AshPile
            { .local = 0x0003280A, .plugin = DragonbornPlugin },  // DLC2AshSpawnAshPile
            { .local = 0x00023F83, .plugin = DragonbornPlugin },  // DLC2HMDaedraAshPile
        });
        return Ids;
    }

    // 干尸/裹尸/烧焦尸体等"静态尸体"容器（ESM 解析确认）。
    // 与灰烬不同：它们是 CONT 基类的容器，战利品在"基类容器条目 + 运行时容器数据"里，
    // 不依赖关联 Actor。
    [[nodiscard]] std::vector<RE::FormID> const& corpse_object_form_ids()
    {
        static auto const Ids = resolve_form_ids({
            // Skyrim.esm
            { .local = 0x00023969, .plugin = SkyrimPlugin },  // TreasDraugrAmbushCorpse01
            { .local = 0x0008008D, .plugin = SkyrimPlugin },  // TreasDraugrAmbushCorpseWrapped01
            { .local = 0x0008008E, .plugin = SkyrimPlugin },  // TreasDraugrAmbushCorpseWrapped02
            { .local = 0x0008008F, .plugin = SkyrimPlugin },  // TreasDraugrAmbushCorpse02
            { .local = 0x00080090, .plugin = SkyrimPlugin },  // TreasDraugrAmbushCorpse03
            { .local = 0x00080091, .plugin = SkyrimPlugin },  // TreasDraugrAmbushCorpse04
            { .local = 0x00080092, .plugin = SkyrimPlugin },  // TreasDraugrAmbushCorpse05
            { .local = 0x00080093, .plugin = SkyrimPlugin },  // TreasDraugrAmbushCorpse06
            { .local = 0x00080094, .plugin = SkyrimPlugin },  // TreasDraugrAmbushCorpse07
            { .local = 0x00042745, .plugin = SkyrimPlugin },  // TreasBurntCorpse01
            { .local = 0x00042746, .plugin = SkyrimPlugin },  // TreasBurntCorpse02
            { .local = 0x00042747, .plugin = SkyrimPlugin },  // TreasBurntCorpse03
            { .local = 0x00042748, .plugin = SkyrimPlugin },  // TreasBurntCorpse04
            { .local = 0x00042749, .plugin = SkyrimPlugin },  // TreasBurntCorpse05
            { .local = 0x000DD060, .plugin = SkyrimPlugin },  // MQ104BurntCorpse03
            { .local = 0x000DD061, .plugin = SkyrimPlugin },  // MQ104BurntCorpse04
            { .local = 0x000BAD05, .plugin = SkyrimPlugin },  // TreasCorpseMammoth
            { .local = 0x000D4FFD, .plugin = SkyrimPlugin },  // POICorpseFrozenMammoth
            { .local = 0x00020668, .plugin = SkyrimPlugin },  // TreasSpiderWebCorpseHuman
            { .local = 0x000C674B, .plugin = SkyrimPlugin },  // defaultGhostCorpse
            { .local = 0x000E7A36, .plugin = SkyrimPlugin },  // dunGeirmundCorpse
            { .local = 0x00018E73, .plugin = SkyrimPlugin },  // MS05_SvaknirsCorpse
            { .local = 0x0010EB29, .plugin = SkyrimPlugin },  // wispCorpseContainer
            { .local = 0x00023968, .plugin = SkyrimPlugin },  // DraugrBodyLaying0000 (STAT)
            // DLC
            { .local = 0x0000A904, .plugin = DawnguardPlugin },   // DLC01defaultSoulCorpse
            { .local = 0x00018C3B, .plugin = DragonbornPlugin },  // DLC2TreasDraugrAmbushCorpseWrapped01EMPTY
        });
        return Ids;
    }

    [[nodiscard]] bool is_ref_form_in(RE::TESObjectREFR const* a_ref, std::vector<RE::FormID> const& a_ids)
    {
        if (!a_ref)
            return false;
        
        RE::TESBoundObject const* base = a_ref->GetBaseObject();
        if (!base)
            return false;
        
        RE::FormID const id = base->GetFormID();
        return std::ranges::all_of(a_ids, [&](RE::FormID const& a_form)
        {
            if (id == a_form)
                return true;
            return false;
        });
    }

    [[nodiscard]] bool is_ash_pile_ref(RE::TESObjectREFR* a_ref)
    {
        return is_ref_form_in(a_ref, ash_pile_form_ids());
    }

    [[nodiscard]] bool is_corpse_object_ref(RE::TESObjectREFR* a_ref)
    {
        return is_ref_form_in(a_ref, corpse_object_form_ids());
    }

    // 按 FormID 去重的状态日志：同一 form_id 的内容变化时才输出（首次必输出），
    // 避免每 0.5s 刷屏。一次性跳过诊断与灰烬堆状态诊断共用这一个权威实现。
    void log_state_once(RE::FormID a_form_id, std::string_view a_detail)
    {
        static std::mutex mtx;
        static std::vector<RE::FormID> seen;
        static std::vector<std::string> details;
        {
            std::lock_guard lock(mtx);
            for (std::size_t i = 0; i < seen.size(); ++i)
            {
                if (seen[i] != a_form_id)
                    continue;
                if (details[i] == a_detail)
                    return;
                
                details[i] = std::string(a_detail);
                break;
            }
            if (std::ranges::find(seen, a_form_id) == seen.end())
            {
                seen.push_back(a_form_id);
                details.emplace_back(a_detail);
            }
        }
        logger::info("{}", a_detail);
    }

    // 一次性诊断：被"看起来还活着"过滤器排除的 Actor（转发到 log_state_once）
    void log_once_skip(RE::Actor* a_actor, std::string_view a_reason)
    {
        log_state_once(
            a_actor->GetFormID(),
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
            fmt::format(
                "Ash Pile {:08X} base {:08X}: {}",
                a_ref->GetFormID(),
                a_ref->GetBaseObject() ? a_ref->GetBaseObject()->GetFormID() : 0,
                a_detail));
    }

    // 查找与灰烬堆关联的 Actor（原始尸体）。
    // 原版机制：Actor 化为灰烬时，灰烬堆自身带 ExtraAshPileRef（kAshPileRef, 0x85）
    // 指向原始 Actor 的句柄（日志里 ash_link 字段）；原始 Actor 的 ExtraDataList 上
    // 也有反向链接。物品挂在原始 Actor 的仓库上——打开灰烬堆时引擎展示的就是它，
    // 这正是"堆本身读不到库存但能搜刮到东西"的原因。
    [[nodiscard]] RE::Actor* find_ash_pile_owner(RE::TESObjectREFR* a_pile)
    {
        if (!a_pile)
            return nullptr;
        
        // 优先：灰烬堆自己的 ExtraAshPileRef → 原始 Actor
        RE::ObjectRefHandle const pile_link = a_pile->extraList.GetAshPileRef();
        if (pile_link)
        {
            if (RE::NiPointer<RE::TESObjectREFR> const ref = pile_link.get())
            {
                if (RE::Actor* actor = ref->As<RE::Actor>())
                    return actor;
            }
        }
        // 兜底：过程列表里 ExtraAshPileRef == 本堆句柄 的 Actor
        uint32_t const pile_handle = a_pile->GetHandle().native_handle();
        RE::ProcessLists* process_lists = RE::ProcessLists::GetSingleton();
        if (!process_lists)
            return nullptr;
        
        auto const find_in = [&](RE::BSTArray<RE::ActorHandle> const& a_list) -> RE::Actor* 
        {
            for (RE::ActorHandle const& handle : a_list)
            {
                const RE::NiPointer<RE::Actor> actor = handle.get();
                if (actor && actor->extraList.GetAshPileRef().native_handle() == pile_handle)
                    return actor.get();
            }
            return nullptr;
        };
        
        if (RE::Actor* actor = find_in(process_lists->highActorHandles))
            return actor;
        if (RE::Actor* actor = find_in(process_lists->middleHighActorHandles))
            return actor;
        if (RE::Actor* actor = find_in(process_lists->middleLowActorHandles))
            return actor;
        return find_in(process_lists->lowActorHandles);
    }

    // 静态尸体容器：基类容器条目（CONT 记录自带战利品）+ 运行时容器数据（只读）。
    // 与灰烬堆不同，这类物体的仓库规则是普通容器规则，不依赖关联 Actor。
    [[nodiscard]] bool has_container_loot(RE::TESObjectREFR* a_ref)
    {
        if (!a_ref)
            return false;
        
        if (RE::TESContainer* container = a_ref->GetContainer())
        {
            if (container->numContainerObjects > 0)
                return true;
        }
        if (RE::InventoryChanges* changes = a_ref->GetInventoryChanges(true))
        {
            if (changes->entryList && !changes->entryList->empty())
                return true;
        }
        return false;
    }
}

namespace CorpseFinder
{
    void scan()
    {
        RE::TES* tes = RE::TES::GetSingleton();
        RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton();
        if (!tes || !player)
            return;

        Config::Settings const& cfg = Config::get();
        RE::NiPoint3 const player_pos = player->GetPosition();

        std::vector<CorpseEntry> found;
        found.reserve(64);
        
        auto const consider = [&](RE::Actor* a_actor) 
        {
            if (!a_actor || a_actor == player)
                return;
            if (a_actor->IsDisabled() || a_actor->IsDeleted())
                return;

            // 距离已由 ForEachReferenceInRange 保证在 max_distance 内；这里只算距离用于淡出
            RE::NiPoint3 const pos = a_actor->GetPosition();
            float const dist = (pos - player_pos).Length();

            // ---- 死亡判定（不直接信引擎虚函数 IsDead()）----
            // 实测本机 AE（1.6.649）上 IsDead() 的虚表分发对几乎所有 Actor 都返回
            // true（连 Belethor、Gerdur 这些活人也报死），不可用——疑似该 CommonLibSSE
            // 版本对 AE 的 Actor 虚表槽位标错。这里直接读 life_state 位域：
            // 只有 kDead 才算候选尸体，kDying（倒地濒死，随从/召唤物还会爬起来）不算。
            RE::ACTOR_LIFE_STATE const life_state = a_actor->AsActorState()->GetLifeState();
            if (life_state != RE::ACTOR_LIFE_STATE::kDead)
            {
                if (life_state == RE::ACTOR_LIFE_STATE::kDying)
                    log_once_skip(a_actor, "downed/bleedout (kDying), may get up");
                return;
            }

            if (a_actor->IsReanimated())
                return;
            if (a_actor->IsGhost())
                return;
            if (!a_actor->Is3DLoaded())
                return;
            

            // 只显示仍有余下可搜刮物品的尸体
            if (a_actor->GetInventory().empty())
                return;

            CorpseEntry entry;
            entry.form_id = a_actor->GetFormID();
            entry.anchor = pos;
            entry.anchor.z += 40.0f;  // 默认锚点抬高到尸体中部
            entry.radius = 60.0f;
            entry.distance = dist;

            RE::NiPoint3 b_min, b_max;
            bool const ragdoll = a_actor->IsInRagdollState();
            if (compute_bounds(a_actor, ragdoll, b_min, b_max, entry.obb_corners, entry.has_obb, entry.bounds_from_collision))
            {
                entry.bound_min = b_min;
                entry.bound_max = b_max;
                // 锚点/半径改为取包围盒本身，保证投影与盒子一致
                entry.anchor = { (b_min.x + b_max.x) * 0.5f, (b_min.y + b_max.y) * 0.5f, (b_min.z + b_max.z) * 0.5f };
                entry.radius = std::max((b_max - b_min).Length() * 0.5f, 10.0f);
            } 
            else if (const RE::NiAVObject* node = a_actor->Get3D())
            {
                // 兜底：根节点包围球
                RE::NiBound const& bound = node->worldBound;
                if (bound.radius > 0.0f && bound.radius < 10000.0f)
                {
                    entry.anchor = bound.center;
                    entry.radius = bound.radius;
                }
            }

            found.push_back(entry);
        };

        auto const consider_object = [&](RE::TESObjectREFR* a_ref)
        {
            bool const is_ash = is_ash_pile_ref(a_ref);
            bool const is_corpse_obj = is_ash ? false : is_corpse_object_ref(a_ref);
            if (!is_ash && !is_corpse_obj)
                return;

            // 诊断：只读检查容器状态（不创建任何东西）
            bool const has_extra = a_ref->extraList.HasType<RE::ExtraContainerChanges>();
            RE::InventoryChanges const* changes = a_ref->GetInventoryChanges(true);
            uint32_t const entry_count = changes && changes->entryList ? changes->entryList->size() : 0;
            // 引擎自身视角（地址库重定位，非虚表）：容器 UI 用的条目计数。
            // 参数变体都试一遍，避免语义/调用方式偏差；任何一个 > 0 都视为可搜刮。
            int const cnt_view = std::max(a_ref->GetInventoryItemCount(true, false), 0);
            int const cnt_self = std::max(a_ref->GetInventoryItemCount(false, false), 0);
            int const cnt_play = std::max(a_ref->GetInventoryItemCount(false, true), 0);
            uint32_t const ash_link = a_ref->extraList.GetAshPileRef().native_handle();
            bool const base_loot = is_corpse_obj && has_container_loot(a_ref);

            bool lootable = entry_count > 0 || cnt_view > 0 || cnt_self > 0 || cnt_play > 0 || base_loot;
            RE::FormID owner_id = 0;
            if (!lootable)
            {
                // 兜底：物品可能挂在 ExtraAshPileRef 关联的 Actor 上
                if (RE::Actor* owner = find_ash_pile_owner(a_ref))
                {
                    owner_id = owner->GetFormID();
                    lootable = !owner->GetInventory().empty();
                }
            }
            log_ash_pile_state(
                a_ref,
                fmt::format(
                    "{} base_loot={} has_extra={} entries={} cnt_view={} cnt_self={} cnt_play={} ash_link={:08X} owner={:08X} lootable={}",
                    is_ash ? "AshPile" : "CorpseObj",
                    base_loot,
                    has_extra,
                    entry_count,
                    cnt_view,
                    cnt_self,
                    cnt_play,
                    ash_link,
                    owner_id,
                    lootable));

            // 只认还有东西可搜刮的
            if (!lootable)
                return;

            CorpseEntry entry;
            entry.form_id = a_ref->GetFormID();
            entry.anchor = a_ref->GetPosition();
            entry.anchor.z += 15.0f;
            entry.radius = 40.0f;
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
            entry.distance = (entry.anchor - player_pos).Length();
            entry.is_ash_pile = is_ash;
            entry.is_static_corpse = is_corpse_obj;
            found.push_back(entry);
        };

        // 单次半径扫描：Actor 尸体 + 灰烬堆 + 静态尸体（干尸/裹尸等）一次遍历完成。
        // 用 TES::ForEachReferenceInRange（内部空间/外部网格/天空 cell 全覆盖，
        // 且按平方距离精确裁剪），不再遍历全量过程列表。
        tes->ForEachReferenceInRange(player, cfg.max_distance, [&](RE::TESObjectREFR* a_ref) -> RE::BSContainer::ForEachResult 
        {
            if (RE::Actor* actor = a_ref->As<RE::Actor>())
                consider(actor);
            else if (is_ash_pile_ref(a_ref) || is_corpse_object_ref(a_ref))
                consider_object(a_ref);
            
            return RE::BSContainer::ForEachResult::kContinue;
        });

        {
            std::lock_guard lock(g_mutex);
            g_corpses = std::move(found);
        }

        // 诊断：打印当前列表（LookupForm 传完整 FormID 在 Skyrim.esm 上等价于
        // 全库查找；临时 ref（FFxxxxxx）查不到时只打 ID）
        for (CorpseEntry const& corpse : g_corpses)
        {
            RE::TESDataHandler* dh = RE::TESDataHandler::GetSingleton();
            if (!dh)
                break;
            RE::TESForm* form = dh->LookupForm(corpse.form_id, SkyrimPlugin);
            if (form)
            {
                if (RE::Actor* actor = form->As<RE::Actor>())
                    logger::info("Corpse {:08X} ({}) added to list", actor->GetFormID(), actor->GetDisplayFullName());
                else
                    logger::info("Corpse {:08X} (non-actor: {}) added to list", corpse.form_id, form->GetFormEditorID());
            } 
            else
                logger::info("Corpse {:08X} (temp/unresolved) added to list", corpse.form_id);
        }

        std::size_t ash_count = 0;
        std::size_t corpse_count = 0;
        for (auto const& e : g_corpses)
        {
            if (e.is_ash_pile)
                ++ash_count;
            else if (e.is_static_corpse)
                ++corpse_count;
        }
        logger::info("Corpse scan found {} searchable corpses ({} ash piles, {} static corpses)", g_corpses.size(), ash_count, corpse_count);
    }

    std::vector<CorpseEntry> snapshot()
    {
        std::lock_guard lock(g_mutex);
        return g_corpses;
    }
}
