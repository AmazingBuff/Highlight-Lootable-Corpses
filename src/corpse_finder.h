#pragma once

#include <cstdint>
#include <vector>

namespace RE
{
    class TESObjectREFR;
}

namespace CorpseFinder
{
    struct CorpseEntry
    {
        RE::FormID form_id{};            // Actor / 灰烬堆的 FormID
        RE::NiPoint3 anchor{};           // 世界坐标锚点（尸体/灰烬堆中部，用于投影）
        RE::NiPoint3 bound_min{};        // 3D 世界包围盒最小角（AABB）
        RE::NiPoint3 bound_max{};        // 3D 世界包围盒最大角（AABB）
        float radius{ 60.0f };           // 世界包围盒半径（兜底用）
        float distance{ 0.0f };          // 与玩家的距离
        bool is_ash_pile{ false };       // 是否为灰烬堆（被复活的尸体再次死亡后转化）
        bool is_static_corpse{ false };  // 是否为静态尸体（干尸/裹尸/烧焦尸体等容器物体）

        // 碰撞盒（OBB）的世界坐标 8 角点。has_obb=true 时渲染器画与碰撞盒一致的 3D 线框盒；
        // 否则退化为 AABB 屏幕矩形。角点顺序：bit0=x(大)，bit1=y(大)，bit2=z(大)。
        bool has_obb{ false };
        RE::NiPoint3 obb_corners[8]{};
        bool bounds_from_collision{ false };  // 诊断：包围盒是否来自 Havok 碰撞体（否则为几何兜底）

        // 战利品筛选（扫描期由 LootFilter::evaluate 计算）：命中的价值分类位掩码与最高单件价值
        std::uint16_t loot_categories{ 0 };
        std::int32_t best_item_value{ 0 };
    };

    // 在游戏主线程上执行：扫描已加载的 Actor，找出"已死亡且仍有可搜刮物品"的尸体；
    // 同时按配置半径直接查询带库存的灰烬堆（被复活的尸体再次死亡后转化）
    // 与静态尸体（干尸/裹尸/烧焦尸体等容器物体）
    void scan();

    // 任意线程安全调用：取回最近一次扫描的快照
    [[nodiscard]] std::vector<CorpseEntry> snapshot();

    // 灰烬堆 → 原始 Actor：堆自身带 ExtraAshPileRef 指向原始 Actor（优先），
    // 兜底查过程列表中 ExtraAshPileRef 指向本堆的 Actor。
    // searched_corpses 模块跨翻译单元使用（标记/查询的灰烬堆双向关联）
    [[nodiscard]] RE::Actor* find_ash_pile_owner(RE::TESObjectREFR* a_pile);

    // kDataLoaded/kNewGame/kPostLoadGame 时调用：确保库存评估缓存的失效监听
    // （TESContainerChangedEvent sink）已注册（幂等），并清空评估缓存——
    // 读档/新游戏后旧评估全部作废
    void reset_loot_cache();
}
