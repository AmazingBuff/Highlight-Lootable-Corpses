#pragma once

#include "filter/loot_filter.h"

PLUGIN_NAMESPACE_BEGIN

class CorpseScan
{
public:
    CorpseScan() = delete;
    ~CorpseScan() = delete;
    CorpseScan(CorpseScan const&) = delete;
    CorpseScan(CorpseScan const&&) = delete;
    CorpseScan operator=(CorpseScan&) = delete;
    CorpseScan operator=(CorpseScan&&) = delete;

    struct CorpseInfo
    {
        RE::FormID form_id;              // Actor / 灰烬堆的 FormID
        RE::NiPoint3 anchor;             // 世界坐标锚点（尸体/灰烬堆中部，用于投影）
        RE::NiPoint3 bound_min;          // 3D 世界包围盒最小角（AABB）
        RE::NiPoint3 bound_max;          // 3D 世界包围盒最大角（AABB）
        float radius;                    // 世界包围盒半径（兜底用）
        float distance;                  // 与玩家的距离
        bool is_ash_pile;                // 是否为灰烬堆（被复活的尸体再次死亡后转化）
        bool is_static_corpse;           // 是否为静态尸体（干尸/裹尸/烧焦尸体等容器物体）

        // 碰撞盒（OBB）的世界坐标 8 角点。has_obb=true 时渲染器画与碰撞盒一致的 3D 线框盒；
        // 否则退化为 AABB 屏幕矩形。角点顺序：bit0=x(大)，bit1=y(大)，bit2=z(大)。
        bool has_obb;
        RE::NiPoint3 obb_corners[8];
        bool bounds_from_collision;  // 诊断：包围盒是否来自 Havok 碰撞体（否则为几何兜底）

        // 战利品筛选（扫描期由 LootFilter::evaluate 计算）：命中的价值分类位掩码与最高单件价值
        RE::stl::enumeration<LootFilter::Category> loot_categories;
        std::int32_t best_item_value;
    };

    static void search();
    [[nodiscard]] static std::vector<CorpseInfo> snapshot();
};

PLUGIN_NAMESPACE_END
