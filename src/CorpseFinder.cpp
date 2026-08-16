#include "PCH.h"
#include "CorpseFinder.h"
#include "Config.h"

namespace
{
    constexpr char const* kSkyrimPlugin = "Skyrim.esm";

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

    [[nodiscard]] float HkX(RE::hkVector4 const& a_v) { return a_v.quad.m128_f32[0]; }
    [[nodiscard]] float HkY(RE::hkVector4 const& a_v) { return a_v.quad.m128_f32[1]; }
    [[nodiscard]] float HkZ(RE::hkVector4 const& a_v) { return a_v.quad.m128_f32[2]; }

    [[nodiscard]] RE::NiPoint3 HkToNi(RE::hkVector4 const& a_v)
    {
        return { HkX(a_v), HkY(a_v), HkZ(a_v) };
    }

    // Havok 世界尺度逆：米 → 游戏单位（引擎全局，Precision 同款地址）
    [[nodiscard]] float WorldScaleInverse()
    {
        static REL::Relocation<float*> g_worldScaleInverse{ RELOCATION_ID(230692, 187407) };
        auto* scale = g_worldScaleInverse.get();
        return scale ? *scale : 70.0f;
    }

    // Havok 世界变换（hkTransform，米）→ NiTransform（游戏单位）
    [[nodiscard]] RE::NiTransform HkTransformToNi(RE::hkTransform const& a_t)
    {
        RE::NiTransform out;
        out.scale = 1.0f;
        auto const& r = a_t.rotation;
        out.rotate.entry[0][0] = HkX(r.col0);
        out.rotate.entry[0][1] = HkX(r.col1);
        out.rotate.entry[0][2] = HkX(r.col2);
        out.rotate.entry[1][0] = HkY(r.col0);
        out.rotate.entry[1][1] = HkY(r.col1);
        out.rotate.entry[1][2] = HkY(r.col2);
        out.rotate.entry[2][0] = HkZ(r.col0);
        out.rotate.entry[2][1] = HkZ(r.col1);
        out.rotate.entry[2][2] = HkZ(r.col2);
        float const s = WorldScaleInverse();
        out.translate = { HkX(a_t.translation) * s, HkY(a_t.translation) * s, HkZ(a_t.translation) * s };
        return out;
    }

    void ExpandAabb(RE::NiPoint3& a_min, RE::NiPoint3& a_max, RE::NiPoint3 const& a_p)
    {
        a_min.x = std::min(a_min.x, a_p.x);
        a_min.y = std::min(a_min.y, a_p.y);
        a_min.z = std::min(a_min.z, a_p.z);
        a_max.x = std::max(a_max.x, a_p.x);
        a_max.y = std::max(a_max.y, a_p.y);
        a_max.z = std::max(a_max.z, a_p.z);
    }

    // 单个 Havok 刚体的世界 AABB（GetAabbWorldspace，havok 米 → 游戏单位）
    [[nodiscard]] bool AddRigidBodyAabb(RE::bhkRigidBody* a_body, RE::NiPoint3& a_min, RE::NiPoint3& a_max)
    {
        if (!a_body)
        {
            return false;
        }
        RE::hkAabb aabb;
        a_body->GetAabbWorldspace(aabb);
        float const s = WorldScaleInverse();
        RE::NiPoint3 const mn{ HkX(aabb.min) * s, HkY(aabb.min) * s, HkZ(aabb.min) * s };
        RE::NiPoint3 const mx{ HkX(aabb.max) * s, HkY(aabb.max) * s, HkZ(aabb.max) * s };
        if (mn.x > mx.x || mn.y > mx.y || mn.z > mx.z)
        {
            return false;
        }
        ExpandAabb(a_min, a_max, mn);
        ExpandAabb(a_min, a_max, mx);
        return true;
    }

    // 递归遍历 3D 节点树，收集"已加入 Havok 世界"的碰撞对象（普通状态/灰烬堆）：
    // - 世界 AABB：GetAabbWorldspace 并集；
    // - OBB：体积最大的盒形碰撞体（通常是 Actor 根部碰撞盒）的世界 8 角点，
    //   由形状半边长（米）与身体世界变换算出。
    void ExpandCollisionObjects(
        RE::NiAVObject* a_node,
        RE::NiPoint3& a_min,
        RE::NiPoint3& a_max,
        std::size_t& a_count,
        RE::NiPoint3* a_obbCorners,
        bool& a_hasOBB,
        float& a_bestVolume)
    {
        if (!a_node)
        {
            return;
        }
        if (auto* colObj = a_node->GetCollisionObject())
        {
            if (auto* body = colObj->GetRigidBody())
            {
                if (auto* rb = body->GetRigidBody())
                {
                    if (rb->world)
                    {  // 在 Havok 世界里 → 变换实时有效
                        if (AddRigidBodyAabb(body, a_min, a_max))
                        {
                            ++a_count;
                            if (a_obbCorners)
                            {
                                auto const* shape = rb->GetShape();
                                if (shape && shape->type == RE::hkpShapeType::kBox)
                                {
                                    auto const* box = static_cast<RE::hkpBoxShape const*>(shape);
                                    float const s = WorldScaleInverse();
                                    auto const he = HkToNi(box->halfExtents) * s;
                                    float const vol = he.x * he.y * he.z;
                                    if (vol > a_bestVolume)
                                    {
                                        a_bestVolume = vol;
                                        auto const world = HkTransformToNi(rb->motion.motionState.transform);
                                        for (int i = 0; i < 8; ++i)
                                        {
                                            float const sx = (i & 1) ? he.x : -he.x;
                                            float const sy = (i & 2) ? he.y : -he.y;
                                            float const sz = (i & 4) ? he.z : -he.z;
                                            a_obbCorners[i] = world * RE::NiPoint3{ sx, sy, sz };
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
        if (auto* node = a_node->AsNode())
        {
            for (auto const& child : node->children)
            {
                if (child)
                {
                    ExpandCollisionObjects(child.get(), a_min, a_max, a_count, a_obbCorners, a_hasOBB, a_bestVolume);
                }
            }
        }
    }

    // ragdoll 尸体：Precision 同款 —— 取动画图里 ragdoll 实例的所有刚体 AABB 并集。
    // 这些刚体（hkaRagdollInstance::rigidBodies）就是尸体各部位的实际碰撞体，
    // 与躺姿完全一致。
    [[nodiscard]] bool ComputeRagdollBounds(RE::Actor* a_actor, RE::NiPoint3& a_min, RE::NiPoint3& a_max)
    {
        RE::BSAnimationGraphManagerPtr animGraphManager;
        if (!a_actor->GetAnimationGraphManager(animGraphManager))
        {
            return false;
        }

        std::size_t count = 0;
        RE::BSSpinLockGuard lock(animGraphManager->GetRuntimeData().updateLock);
        for (auto const& graph : animGraphManager->graphs)
        {
            if (!graph)
            {
                continue;
            }
            auto& driver = graph.get()->characterInstance.ragdollDriver;
            if (!driver)
            {
                continue;
            }
            auto* ragdoll = driver->ragdoll;
            if (!ragdoll)
            {
                continue;
            }
            for (auto* rb : ragdoll->rigidBodies)
            {
                if (!rb)
                {
                    continue;
                }
                // hkpRigidBody::userData 指向它的 bhkRigidBody 包装（Precision 同款用法）
                auto* wrapper = reinterpret_cast<RE::bhkRigidBody*>(rb->userData);
                if (AddRigidBodyAabb(wrapper, a_min, a_max))
                {
                    ++count;
                }
            }
        }
        return count > 0;
    }

    // 兜底：只取"几何节点"的 worldBound 包围球累加。
    // 相比旧实现（所有节点都累加，根节点的大球把盒子撑大一圈），
    // 几何节点球更贴合尸体实际轮廓。
    void ExpandGeometryBounds(RE::NiAVObject* a_node, RE::NiPoint3& a_min, RE::NiPoint3& a_max)
    {
        if (!a_node)
        {
            return;
        }
        if (a_node->AsGeometry())
        {
            auto const& bound = a_node->worldBound;
            if (bound.radius > 0.0f && bound.radius < 100000.0f)
            {
                auto const& c = bound.center;
                ExpandAabb(a_min, a_max, { c.x - bound.radius, c.y - bound.radius, c.z - bound.radius });
                ExpandAabb(a_min, a_max, { c.x + bound.radius, c.y + bound.radius, c.z + bound.radius });
            }
        }
        if (auto* node = a_node->AsNode())
        {
            for (auto const& child : node->children)
            {
                if (child)
                {
                    ExpandGeometryBounds(child.get(), a_min, a_max);
                }
            }
        }
    }

    // 综合入口：ragdoll → ragdoll 刚体；否则 → 3D 树上的碰撞对象；最后几何兜底。
    // 返回 true 表示得到了有效的世界 AABB。
    [[nodiscard]] bool ComputeBounds(
        RE::TESObjectREFR* a_ref,
        bool a_ragdoll,
        RE::NiPoint3& a_min,
        RE::NiPoint3& a_max,
        RE::NiPoint3* a_obbCorners,
        bool& a_hasOBB,
        bool& a_fromCollision)
    {
        a_hasOBB = false;
        a_fromCollision = false;
        if (!a_ref)
        {
            return false;
        }
        auto* node = a_ref->Get3D();
        if (!node)
        {
            return false;
        }

        RE::NiPoint3 mn{ FLT_MAX, FLT_MAX, FLT_MAX };
        RE::NiPoint3 mx{ -FLT_MAX, -FLT_MAX, -FLT_MAX };

        if (a_ragdoll)
        {
            if (auto* actor = a_ref->As<RE::Actor>())
            {
                if (ComputeRagdollBounds(actor, mn, mx))
                {
                    a_min = mn;
                    a_max = mx;
                    a_fromCollision = true;
                    return true;
                }
                mn = RE::NiPoint3{ FLT_MAX, FLT_MAX, FLT_MAX };
                mx = RE::NiPoint3{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
            }
        } else
        {
            std::size_t count = 0;
            float bestVol = 0.0f;
            ExpandCollisionObjects(node, mn, mx, count, a_obbCorners, a_hasOBB, bestVol);
            if (count > 0 && mn.x <= mx.x && mn.y <= mx.y && mn.z <= mx.z)
            {
                a_min = mn;
                a_max = mx;
                a_fromCollision = true;
                return true;
            }
            mn = RE::NiPoint3{ FLT_MAX, FLT_MAX, FLT_MAX };
            mx = RE::NiPoint3{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
            a_hasOBB = false;
        }

        ExpandGeometryBounds(node, mn, mx);
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
        std::uint32_t local;
        char const* plugin;
    };

    [[nodiscard]] std::vector<RE::FormID> ResolveFormIDs(std::initializer_list<StaticFormID> a_forms)
    {
        std::vector<RE::FormID> out;
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (dh)
        {
            for (auto const& f : a_forms)
            {
                auto const id = dh->LookupFormID(f.local, f.plugin);
                if (id != 0)
                {
                    out.push_back(id);
                }
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
    [[nodiscard]] std::vector<RE::FormID> const& AshPileFormIDs()
    {
        static auto const ids = ResolveFormIDs({
            { 0x0000001B, kSkyrimPlugin }, { 0x00000022, kSkyrimPlugin },
            { 0x00101048, kSkyrimPlugin }, { 0x001069E4, kSkyrimPlugin },
            { 0x0010C649, kSkyrimPlugin }, { 0x0010D6EF, kSkyrimPlugin },
            { 0x0000A905, "Dawnguard.esm" },   // DLC01DefaultAshPileSoul (Soul Ember)
            { 0x0000BDCE, "Dawnguard.esm" },   // DLC01DefaultAshPileEnemies
            { 0x0000FC74, "Dawnguard.esm" },   // DLC1dunHarkonAshPile
            { 0x00003522, "Dawnguard.esm" },   // DLC1_WESC08AshPile
            { 0x0003280A, "Dragonborn.esm" },  // DLC2AshSpawnAshPile
            { 0x00023F83, "Dragonborn.esm" },  // DLC2HMDaedraAshPile
        });
        return ids;
    }

    // 干尸/裹尸/烧焦尸体等"静态尸体"容器（ESM 解析确认）。
    // 与灰烬不同：它们是 CONT 基类的容器，战利品在"基类容器条目 + 运行时容器数据"里，
    // 不依赖关联 Actor。
    [[nodiscard]] std::vector<RE::FormID> const& CorpseObjectFormIDs()
    {
        static auto const ids = ResolveFormIDs({
            // Skyrim.esm
            { 0x00023969, kSkyrimPlugin },  // TreasDraugrAmbushCorpse01
            { 0x0008008D, kSkyrimPlugin },  // TreasDraugrAmbushCorpseWrapped01
            { 0x0008008E, kSkyrimPlugin },  // TreasDraugrAmbushCorpseWrapped02
            { 0x0008008F, kSkyrimPlugin },  // TreasDraugrAmbushCorpse02
            { 0x00080090, kSkyrimPlugin },  // TreasDraugrAmbushCorpse03
            { 0x00080091, kSkyrimPlugin },  // TreasDraugrAmbushCorpse04
            { 0x00080092, kSkyrimPlugin },  // TreasDraugrAmbushCorpse05
            { 0x00080093, kSkyrimPlugin },  // TreasDraugrAmbushCorpse06
            { 0x00080094, kSkyrimPlugin },  // TreasDraugrAmbushCorpse07
            { 0x00042745, kSkyrimPlugin },  // TreasBurntCorpse01
            { 0x00042746, kSkyrimPlugin },  // TreasBurntCorpse02
            { 0x00042747, kSkyrimPlugin },  // TreasBurntCorpse03
            { 0x00042748, kSkyrimPlugin },  // TreasBurntCorpse04
            { 0x00042749, kSkyrimPlugin },  // TreasBurntCorpse05
            { 0x000DD060, kSkyrimPlugin },  // MQ104BurntCorpse03
            { 0x000DD061, kSkyrimPlugin },  // MQ104BurntCorpse04
            { 0x000BAD05, kSkyrimPlugin },  // TreasCorpseMammoth
            { 0x000D4FFD, kSkyrimPlugin },  // POICorpseFrozenMammoth
            { 0x00020668, kSkyrimPlugin },  // TreasSpiderWebCorpseHuman
            { 0x000C674B, kSkyrimPlugin },  // defaultGhostCorpse
            { 0x000E7A36, kSkyrimPlugin },  // dunGeirmundCorpse
            { 0x00018E73, kSkyrimPlugin },  // MS05_SvaknirsCorpse
            { 0x0010EB29, kSkyrimPlugin },  // wispCorpseContainer
            { 0x00023968, kSkyrimPlugin },  // DraugrBodyLaying0000 (STAT)
            // DLC
            { 0x0000A904, "Dawnguard.esm" },   // DLC01defaultSoulCorpse
            { 0x00018C3B, "Dragonborn.esm" },  // DLC2TreasDraugrAmbushCorpseWrapped01EMPTY
        });
        return ids;
    }

    [[nodiscard]] bool IsRefFormIn(RE::TESObjectREFR const* a_ref, std::vector<RE::FormID> const& a_ids)
    {
        if (!a_ref)
        {
            return false;
        }
        auto const* base = a_ref->GetBaseObject();
        if (!base)
        {
            return false;
        }
        auto const id = base->GetFormID();
        for (auto const fid : a_ids)
        {
            if (id == fid)
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool IsAshPileRef(RE::TESObjectREFR* a_ref)
    {
        return IsRefFormIn(a_ref, AshPileFormIDs());
    }

    [[nodiscard]] bool IsCorpseObjectRef(RE::TESObjectREFR* a_ref)
    {
        return IsRefFormIn(a_ref, CorpseObjectFormIDs());
    }

    // 按 FormID 去重的状态日志：同一 formID 的内容变化时才输出（首次必输出），
    // 避免每 0.5s 刷屏。一次性跳过诊断与灰烬堆状态诊断共用这一个权威实现。
    void LogStateOnce(RE::FormID a_formID, std::string_view a_detail)
    {
        static std::mutex mtx;
        static std::vector<RE::FormID> seen;
        static std::vector<std::string> details;
        {
            std::lock_guard lock(mtx);
            for (std::size_t i = 0; i < seen.size(); ++i)
            {
                if (seen[i] != a_formID)
                {
                    continue;
                }
                if (details[i] == a_detail)
                {
                    return;
                }
                details[i] = std::string(a_detail);
                break;
            }
            if (std::find(seen.begin(), seen.end(), a_formID) == seen.end())
            {
                seen.push_back(a_formID);
                details.push_back(std::string(a_detail));
            }
        }
        logger::info("{}", a_detail);
    }

    // 一次性诊断：被"看起来还活着"过滤器排除的 Actor（转发到 LogStateOnce）
    void LogOnceSkip(RE::Actor* a_actor, std::string_view a_reason)
    {
        LogStateOnce(
            a_actor->GetFormID(),
            fmt::format(
                "Skip non-corpse {:08X} ({}): {}",
                a_actor->GetFormID(),
                a_actor->GetDisplayFullName(),
                a_reason));
    }

    // 灰烬堆/静态尸体状态诊断（状态变化时才记日志，转发到 LogStateOnce）
    void LogAshPileState(RE::TESObjectREFR* a_ref, std::string_view a_detail)
    {
        LogStateOnce(
            a_ref->GetFormID(),
            fmt::format(
                "Ash Pile {:08X} base {:08X}: {}",
                a_ref->GetFormID(),
                a_ref->GetBaseObject() ? a_ref->GetBaseObject()->GetFormID() : 0,
                a_detail));
    }

    // 查找与灰烬堆关联的 Actor（原始尸体）。
    // 原版机制：Actor 化为灰烬时，灰烬堆自身带 ExtraAshPileRef（kAshPileRef, 0x85）
    // 指向原始 Actor 的句柄（日志里 ashLink 字段）；原始 Actor 的 ExtraDataList 上
    // 也有反向链接。物品挂在原始 Actor 的仓库上——打开灰烬堆时引擎展示的就是它，
    // 这正是"堆本身读不到库存但能搜刮到东西"的原因。
    [[nodiscard]] RE::Actor* FindAshPileOwner(RE::TESObjectREFR* a_pile)
    {
        if (!a_pile)
        {
            return nullptr;
        }
        // 优先：灰烬堆自己的 ExtraAshPileRef → 原始 Actor
        auto const pileLink = a_pile->extraList.GetAshPileRef();
        if (pileLink)
        {
            if (auto ref = pileLink.get())
            {
                if (auto* actor = ref->As<RE::Actor>())
                {
                    return actor;
                }
            }
        }
        // 兜底：过程列表里 ExtraAshPileRef == 本堆句柄 的 Actor
        auto const pileHandle = a_pile->GetHandle().native_handle();
        auto* processLists = RE::ProcessLists::GetSingleton();
        if (!processLists)
        {
            return nullptr;
        }
        auto const findIn = [&](RE::BSTArray<RE::ActorHandle> const& a_list) -> RE::Actor* {
            for (auto const& handle : a_list)
            {
                auto actor = handle.get();
                if (actor && actor->extraList.GetAshPileRef().native_handle() == pileHandle)
                {
                    return actor.get();
                }
            }
            return nullptr;
        };
        if (auto* actor = findIn(processLists->highActorHandles))
        {
            return actor;
        }
        if (auto* actor = findIn(processLists->middleHighActorHandles))
        {
            return actor;
        }
        if (auto* actor = findIn(processLists->middleLowActorHandles))
        {
            return actor;
        }
        return findIn(processLists->lowActorHandles);
    }

    // 静态尸体容器：基类容器条目（CONT 记录自带战利品）+ 运行时容器数据（只读）。
    // 与灰烬堆不同，这类物体的仓库规则是普通容器规则，不依赖关联 Actor。
    [[nodiscard]] bool HasContainerLoot(RE::TESObjectREFR* a_ref)
    {
        if (!a_ref)
        {
            return false;
        }
        if (auto* container = a_ref->GetContainer())
        {
            if (container->numContainerObjects > 0)
            {
                return true;
            }
        }
        if (auto* changes = a_ref->GetInventoryChanges(true))
        {
            if (changes->entryList && !changes->entryList->empty())
            {
                return true;
            }
        }
        return false;
    }
}

namespace CorpseFinder
{
    void Scan()
    {
        auto* tes = RE::TES::GetSingleton();
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!tes || !player)
        {
            return;
        }

        auto const& cfg = Config::Get();
        auto const playerPos = player->GetPosition();

        std::vector<CorpseEntry> found;
        found.reserve(64);

        auto const consider = [&](RE::Actor* a_actor) {
            if (!a_actor || a_actor == player)
            {
                return;
            }
            if (a_actor->IsDisabled() || a_actor->IsDeleted())
            {
                return;
            }

            // 距离已由 ForEachReferenceInRange 保证在 maxDistance 内；这里只算距离用于淡出
            auto const pos = a_actor->GetPosition();
            float const dist = (pos - playerPos).Length();

            // ---- 死亡判定（不直接信引擎虚函数 IsDead()）----
            // 实测本机 AE（1.6.649）上 IsDead() 的虚表分发对几乎所有 Actor 都返回
            // true（连 Belethor、Gerdur 这些活人也报死），不可用——疑似该 CommonLibSSE
            // 版本对 AE 的 Actor 虚表槽位标错。这里直接读 lifeState 位域：
            // 只有 kDead 才算候选尸体，kDying（倒地濒死，随从/召唤物还会爬起来）不算。
            auto const lifeState = a_actor->AsActorState()->GetLifeState();
            if (lifeState != RE::ACTOR_LIFE_STATE::kDead)
            {
                if (lifeState == RE::ACTOR_LIFE_STATE::kDying)
                {
                    LogOnceSkip(a_actor, "downed/bleedout (kDying), may get up");
                }
                return;
            }

            if (a_actor->IsReanimated())
            {
                return;
            }
            if (a_actor->IsGhost())
            {
                return;
            }
            if (!a_actor->Is3DLoaded())
            {
                return;
            }

            // 只显示仍有余下可搜刮物品的尸体
            if (a_actor->GetInventory().empty())
            {
                return;
            }

            CorpseEntry entry;
            entry.formID = a_actor->GetFormID();
            entry.anchor = pos;
            entry.anchor.z += 40.0f;  // 默认锚点抬高到尸体中部
            entry.radius = 60.0f;
            entry.distance = dist;

            RE::NiPoint3 bMin, bMax;
            bool const ragdoll = a_actor->IsInRagdollState();
            if (ComputeBounds(a_actor, ragdoll, bMin, bMax, entry.obbCorners, entry.hasOBB, entry.boundsFromCollision))
            {
                entry.boundMin = bMin;
                entry.boundMax = bMax;
                // 锚点/半径改为取包围盒本身，保证投影与盒子一致
                entry.anchor = { (bMin.x + bMax.x) * 0.5f, (bMin.y + bMax.y) * 0.5f, (bMin.z + bMax.z) * 0.5f };
                entry.radius = std::max((bMax - bMin).Length() * 0.5f, 10.0f);
            } else if (auto* node = a_actor->Get3D())
            {
                // 兜底：根节点包围球
                auto const& bound = node->worldBound;
                if (bound.radius > 0.0f && bound.radius < 10000.0f)
                {
                    entry.anchor = bound.center;
                    entry.radius = bound.radius;
                }
            }

            found.push_back(std::move(entry));
        };

        auto const considerObject = [&](RE::TESObjectREFR* a_ref) {
            bool const isAsh = IsAshPileRef(a_ref);
            bool const isCorpseObj = isAsh ? false : IsCorpseObjectRef(a_ref);
            if (!isAsh && !isCorpseObj)
            {
                return;
            }

            // 诊断：只读检查容器状态（不创建任何东西）
            bool const hasExtra = a_ref->extraList.HasType<RE::ExtraContainerChanges>();
            auto* changes = a_ref->GetInventoryChanges(true);
            auto const entryCount = changes && changes->entryList ? changes->entryList->size() : 0;
            // 引擎自身视角（地址库重定位，非虚表）：容器 UI 用的条目计数。
            // 参数变体都试一遍，避免语义/调用方式偏差；任何一个 > 0 都视为可搜刮。
            auto const cntView = std::max(a_ref->GetInventoryItemCount(true, false), 0);
            auto const cntSelf = std::max(a_ref->GetInventoryItemCount(false, false), 0);
            auto const cntPlay = std::max(a_ref->GetInventoryItemCount(false, true), 0);
            auto const ashLink = a_ref->extraList.GetAshPileRef().native_handle();
            bool const baseLoot = isCorpseObj && HasContainerLoot(a_ref);

            bool lootable = entryCount > 0 || cntView > 0 || cntSelf > 0 || cntPlay > 0 || baseLoot;
            RE::FormID ownerID = 0;
            if (!lootable)
            {
                // 兜底：物品可能挂在 ExtraAshPileRef 关联的 Actor 上
                if (auto* owner = FindAshPileOwner(a_ref))
                {
                    ownerID = owner->GetFormID();
                    lootable = !owner->GetInventory().empty();
                }
            }
            LogAshPileState(
                a_ref,
                fmt::format(
                    "{} baseLoot={} hasExtra={} entries={} cntView={} cntSelf={} cntPlay={} ashLink={:08X} owner={:08X} lootable={}",
                    isAsh ? "AshPile" : "CorpseObj",
                    baseLoot,
                    hasExtra,
                    entryCount,
                    cntView,
                    cntSelf,
                    cntPlay,
                    ashLink,
                    ownerID,
                    lootable));

            // 只认还有东西可搜刮的
            if (!lootable)
            {
                return;
            }

            CorpseEntry entry;
            entry.formID = a_ref->GetFormID();
            entry.anchor = a_ref->GetPosition();
            entry.anchor.z += 15.0f;
            entry.radius = 40.0f;
            if (auto* node = a_ref->Get3D())
            {
                auto const& bound = node->worldBound;
                if (bound.radius > 0.0f && bound.radius < 10000.0f)
                {
                    entry.anchor = bound.center;
                    entry.radius = bound.radius;
                }
            }
            RE::NiPoint3 bMin, bMax;
            bool dummyOBB = false;
            bool dummySrc = false;
            if (ComputeBounds(a_ref, false, bMin, bMax, nullptr, dummyOBB, dummySrc))
            {
                entry.boundMin = bMin;
                entry.boundMax = bMax;
            }
            entry.distance = (entry.anchor - playerPos).Length();
            entry.isAshPile = isAsh;
            entry.isStaticCorpse = isCorpseObj;
            found.push_back(std::move(entry));
        };

        // 单次半径扫描：Actor 尸体 + 灰烬堆 + 静态尸体（干尸/裹尸等）一次遍历完成。
        // 用 TES::ForEachReferenceInRange（内部空间/外部网格/天空 cell 全覆盖，
        // 且按平方距离精确裁剪），不再遍历全量过程列表。
        tes->ForEachReferenceInRange(player, cfg.maxDistance, [&](RE::TESObjectREFR* a_ref) -> RE::BSContainer::ForEachResult {
            if (auto* actor = a_ref->As<RE::Actor>())
            {
                consider(actor);
            } else if (IsAshPileRef(a_ref) || IsCorpseObjectRef(a_ref))
            {
                considerObject(a_ref);
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });

        {
            std::lock_guard lock(g_mutex);
            g_corpses = std::move(found);
        }

        // 诊断：打印当前列表（LookupForm 传完整 FormID 在 Skyrim.esm 上等价于
        // 全库查找；临时 ref（FFxxxxxx）查不到时只打 ID）
        for (auto const& g_corpse : g_corpses)
        {
            auto dh = RE::TESDataHandler::GetSingleton();
            if (!dh)
            {
                break;
            }
            auto form = dh->LookupForm(g_corpse.formID, kSkyrimPlugin);
            if (form)
            {
                if (auto* actor = form->As<RE::Actor>())
                {
                    logger::info("Corpse {:08X} ({}) added to list", actor->GetFormID(), actor->GetDisplayFullName());
                } else
                {
                    logger::info("Corpse {:08X} (non-actor: {}) added to list", g_corpse.formID, form->GetFormEditorID());
                }
            } else
            {
                logger::info("Corpse {:08X} (temp/unresolved) added to list", g_corpse.formID);
            }
        }

        std::size_t ashCount = 0;
        std::size_t corpseCount = 0;
        for (auto const& e : g_corpses)
        {
            if (e.isAshPile)
            {
                ++ashCount;
            } else if (e.isStaticCorpse)
            {
                ++corpseCount;
            }
        }
        logger::info("Corpse scan found {} searchable corpses ({} ash piles, {} static corpses)", g_corpses.size(), ashCount, corpseCount);
    }

    std::vector<CorpseEntry> Snapshot()
    {
        std::lock_guard lock(g_mutex);
        return g_corpses;
    }
}
