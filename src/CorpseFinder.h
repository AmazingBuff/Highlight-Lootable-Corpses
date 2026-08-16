#pragma once

#include <vector>

namespace CorpseFinder
{
    struct CorpseEntry
    {
        RE::FormID formID{};           // Actor / 灰烬堆的 FormID
        RE::NiPoint3 anchor{};         // 世界坐标锚点（尸体/灰烬堆中部，用于投影）
        RE::NiPoint3 boundMin{};       // 3D 世界包围盒最小角（AABB）
        RE::NiPoint3 boundMax{};       // 3D 世界包围盒最大角（AABB）
        float radius{ 60.0f };         // 世界包围盒半径（兜底用）
        float distance{ 0.0f };        // 与玩家的距离
        bool isAshPile{ false };       // 是否为灰烬堆（被复活的尸体再次死亡后转化）
        bool isStaticCorpse{ false };  // 是否为静态尸体（干尸/裹尸/烧焦尸体等容器物体）

        // 碰撞盒（OBB）的世界坐标 8 角点。hasOBB=true 时渲染器画与碰撞盒一致的 3D 线框盒；
        // 否则退化为 AABB 屏幕矩形。角点顺序：bit0=x(大)，bit1=y(大)，bit2=z(大)。
        bool hasOBB{ false };
        RE::NiPoint3 obbCorners[8]{};
        bool boundsFromCollision{ false };  // 诊断：包围盒是否来自 Havok 碰撞体（否则为几何兜底）
    };

    // 在游戏主线程上执行：扫描已加载的 Actor，找出"已死亡且仍有可搜刮物品"的尸体；
    // 同时按配置半径直接查询带库存的灰烬堆（被复活的尸体再次死亡后转化）
    // 与静态尸体（干尸/裹尸/烧焦尸体等容器物体）
    void Scan();

    // 任意线程安全调用：取回最近一次扫描的快照
    [[nodiscard]] std::vector<CorpseEntry> Snapshot();
}
