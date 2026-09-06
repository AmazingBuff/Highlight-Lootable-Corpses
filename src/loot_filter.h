//
// Created by AmazingBuff on 2026/08/18.
//

#pragma once

#include <cstdint>
#include <string>

namespace RE
{
    class TESObjectREFR;
}

namespace Config
{
    struct Settings;
}

namespace LootFilter
{
    // 战利品价值分类（位掩码：一具尸体可同时命中多类）
    enum class Category : std::uint16_t
    {
        e_quest = 1 << 0,       // 任务物品（任务别名"任务对象"标记）
        e_key = 1 << 1,         // 钥匙
        e_enchanted = 1 << 2,   // 附魔装备（基底或实例附魔）
        e_valuable = 1 << 3,    // 单件价值 >= 阈值（金币堆按枚数计）
        e_book = 1 << 4,        // 书籍（按 Config::book_filter_mode 限定范围）
        e_consumable = 1 << 5,  // 消耗品（箭矢/炼金材料/灵魂石/药水/卷轴）
    };

    // 扫描期评估结果（游戏线程写入，渲染线程经快照只读）
    struct Result
    {
        bool has_items{ false };        // 合并后仍有 count > 0 的物品：是否可搜刮的唯一权威判据
        std::uint16_t categories{ 0 };  // Category 位或
        std::int32_t best_item_value{ 0 };
    };

    // 评估一具尸体的可搜刮库存（Actor / 灰烬堆关联 Actor / 静态尸体容器）。
    [[nodiscard]] Result evaluate(RE::TESObjectREFR* a_ref);

    // 评估结果所依赖的全部配置字段（分类开关、高价值阈值、书籍模式）编码为一个
    // 64 位快照戳：任一字段变化都会使旧评估结果失效——
    // 缓存层据此在 loot filter 参数修改后对所有尸体重新评估
    [[nodiscard]] std::uint64_t config_stamp(Config::Settings const& a_cfg);

    // 由 Config 分类开关合成的掩码（渲染线程无锁读取，用于过滤与状态统计）
    [[nodiscard]] std::uint16_t enabled_category_mask();

    // 日志用：把分类位掩码格式化为 "quest|enchanted"（无命中返回 "none"）
    [[nodiscard]] std::string category_summary(std::uint16_t a_categories);
}
