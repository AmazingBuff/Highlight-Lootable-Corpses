//
// Created by AmazingBuff on 2026/08/18.
//

#include "pch.h"
#include "loot_filter.h"

#include "config.h"

namespace
{
    // 单件物品价值：金币堆按枚数（每枚 1 金），其余取条目价值（含基底/实例附魔）
    [[nodiscard]] std::int32_t item_value(RE::InventoryEntryData const& a_entry, std::int32_t a_count)
    {
        RE::TESBoundObject* const object = a_entry.object;
        if (!object)
            return 0;
        if (object->IsGold())
            return a_count;
        return a_entry.GetValue();
    }

    // 单件物品分类：返回命中的分类位（可多类同命中）
    [[nodiscard]] std::uint16_t classify_item(
        RE::InventoryEntryData const& a_entry,
        std::int32_t a_count,
        Config::Settings const& a_cfg)
    {
        std::uint16_t cats = 0;
        // 注意：GetObject() 会被 windows.h 的 GetObject 宏展开（GetObjectA），改用公开成员
        RE::TESBoundObject* const object = a_entry.object;
        if (!object)
            return cats;

        auto const set = [&](LootFilter::Category a_category) {
            cats |= static_cast<std::uint16_t>(a_category);
        };

        // 任务物品：任务别名"任务对象"标记（基类容器条目无实例数据时恒为 false）
        if (a_cfg.value_quest_items && a_entry.IsQuestObject())
            set(LootFilter::Category::e_quest);

        RE::FormType const type = object->GetFormType();

        if (a_cfg.value_keys && type == RE::FormType::KeyMaster)
            set(LootFilter::Category::e_key);

        // 附魔：InventoryEntryData::IsEnchanted 同时覆盖基底附魔（TESEnchantableForm）
        // 与实例附魔（ExtraEnchantment），基类容器条目也适用
        if (a_cfg.value_enchanted && a_entry.IsEnchanted())
            set(LootFilter::Category::e_enchanted);

        // 高价值：单件价值 >= 阈值
        if (a_cfg.value_high_value && item_value(a_entry, a_count) >= static_cast<std::int32_t>(a_cfg.high_value_threshold))
            set(LootFilter::Category::e_valuable);

        // 书籍：0=全部书籍（含笔记/信件），1=法术+技能书，2=仅法术书
        if (a_cfg.value_books && type == RE::FormType::Book)
        {
            RE::TESObjectBOOK* const book = object->As<RE::TESObjectBOOK>();
            bool match = true;
            if (a_cfg.book_filter_mode == 1)
                match = book && (book->TeachesSpell() || book->TeachesSkill());
            else if (a_cfg.book_filter_mode == 2)
                match = book && book->TeachesSpell();
            if (match)
                set(LootFilter::Category::e_book);
        }

        // 消耗品：箭矢/炼金材料/药水/卷轴；灵魂石按"仅已填充"选项（需条目带灵魂等级）
        if (a_cfg.value_consumables)
        {
            bool consumable = false;
            switch (type)
            {
            case RE::FormType::Ammo:
            case RE::FormType::Ingredient:
            case RE::FormType::AlchemyItem:
            case RE::FormType::Scroll:
                consumable = true;
                break;
            case RE::FormType::SoulGem:
                consumable = !a_cfg.soul_gem_filled_only || a_entry.GetSoulLevel() != RE::SOUL_LEVEL::kNone;
                break;
            default:
                break;
            }
            if (consumable)
                set(LootFilter::Category::e_consumable);
        }

        return cats;
    }
}

namespace LootFilter
{
    Result evaluate(RE::TESObjectREFR* a_ref)
    {
        Result result;
        if (!a_ref)
            return result;

        Config::Settings const& cfg = Config::get();

        // 合并库存由引擎侧权威实现给出：基类容器条目（CONT/NPC 默认战利品，含 leveled
        // 条目去重）+ 运行时 countDelta。a_noInit=true 保证只读、不创建 InventoryChanges。
        // 被拿走的物品体现为 count <= 0，必须剔除，否则搜刮过的尸体会一直判为有货；
        // 只统计玩家可拿取（GetPlayable）的条目，不可拾取的残留物不算"还有货"。
        //
        // 基类 CNTO 里的升级清单（LVLI）条目按容器阶段区别对待（实测：TreasDraugr
        // 系静态尸体的基类容器全是 LVLI，搜空后 count=1 占位条目仍判有货）：
        // - 引擎初始化库存时（打开容器 / Actor 出生）把 LVLI 解析成具体物品，运行时
        //   条目以解析后的具体物品为键——基类 LVLI 条目从此是引擎 UI 永不显示、玩家
        //   拿不到的占位伪物品。已初始化的 ref 必须跳过它们，否则搜空尸体永远判有货。
        // - 未初始化的 ref（从未打开的静态尸体）LVLI 条目代表尚未生成的真实战利品，
        //   保留计数视为有货。该分支不经过 GetPlayable：LVLI 记录 flags=0 时基类实现
        //   应返回 false，但本机虚表分发不可靠（与 IsDead() 同类问题），不可依赖。
        bool const initialized = a_ref->GetInventoryChanges(true) != nullptr;

        for (auto const& [object, data] : a_ref->GetInventory([](RE::TESBoundObject&) { return true; }, true))
        {
            std::int32_t const count = data.first;
            RE::InventoryEntryData const* const entry = data.second.get();
            if (!object || !entry || count <= 0)
                continue;

            if (object->GetFormType() == RE::FormType::LeveledItem)
            {
                // LVLI 无价值/分类贡献可言，两个分支都不进 classify_item
                if (!initialized)
                    result.has_items = true;
                continue;
            }

            if (!object->GetPlayable())
                continue;

            result.has_items = true;
            result.categories |= classify_item(*entry, count, cfg);
            result.best_item_value = std::max(result.best_item_value, item_value(*entry, count));
        }

        return result;
    }

    std::uint16_t enabled_category_mask()
    {
        Config::Settings const& cfg = Config::get();
        std::uint16_t mask = 0;
        auto const set = [&](bool a_enabled, Category a_category) {
            if (a_enabled)
                mask |= static_cast<std::uint16_t>(a_category);
        };
        set(cfg.value_quest_items, Category::e_quest);
        set(cfg.value_keys, Category::e_key);
        set(cfg.value_enchanted, Category::e_enchanted);
        set(cfg.value_high_value, Category::e_valuable);
        set(cfg.value_books, Category::e_book);
        set(cfg.value_consumables, Category::e_consumable);
        return mask;
    }

    std::string category_summary(std::uint16_t a_categories)
    {
        if (a_categories == 0)
            return "none";

        std::string out;
        auto const append = [&](char const* a_name) {
            if (!out.empty())
                out += '|';
            out += a_name;
        };
        if (a_categories & static_cast<std::uint16_t>(Category::e_quest))
            append("quest");
        if (a_categories & static_cast<std::uint16_t>(Category::e_key))
            append("key");
        if (a_categories & static_cast<std::uint16_t>(Category::e_enchanted))
            append("enchanted");
        if (a_categories & static_cast<std::uint16_t>(Category::e_valuable))
            append("valuable");
        if (a_categories & static_cast<std::uint16_t>(Category::e_book))
            append("book");
        if (a_categories & static_cast<std::uint16_t>(Category::e_consumable))
            append("consumable");
        return out;
    }
}
