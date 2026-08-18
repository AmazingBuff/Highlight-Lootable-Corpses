//
// Created by AmazingBuff on 2026/08/18.
//

#include "pch.h"
#include "loot_filter.h"

#include "config.h"

namespace
{
    // 单件物品价值：金币堆按枚数（每枚 1 金），其余取条目价值（含实例附魔）或基础价值
    [[nodiscard]] std::int32_t item_value(RE::TESBoundObject* a_object, RE::InventoryEntryData const* a_entry, std::int32_t a_count)
    {
        if (!a_object)
            return 0;
        if (a_object->IsGold())
            return a_count;
        return a_entry ? a_entry->GetValue() : a_object->GetGoldValue();
    }

    // 单件物品分类：返回命中的分类位（可多类同命中）
    [[nodiscard]] std::uint16_t classify_item(
        RE::TESBoundObject* a_object,
        RE::InventoryEntryData const* a_entry,
        std::int32_t a_count,
        Config::Settings const& a_cfg)
    {
        std::uint16_t cats = 0;
        if (!a_object)
            return cats;

        auto const set = [&](LootFilter::Category a_category) {
            cats |= static_cast<std::uint16_t>(a_category);
        };

        // 任务物品：仅运行时条目带任务别名标记（基础容器条目无从判定）
        if (a_cfg.value_quest_items && a_entry && a_entry->IsQuestObject())
            set(LootFilter::Category::e_quest);

        RE::FormType const type = a_object->GetFormType();

        if (a_cfg.value_keys && type == RE::FormType::KeyMaster)
            set(LootFilter::Category::e_key);

        // 附魔：条目判定覆盖基底附魔（TESEnchantableForm）与实例附魔（ExtraEnchantment）
        if (a_cfg.value_enchanted)
        {
            bool const enchanted = a_entry ?
                                       a_entry->IsEnchanted() :
                                       (a_object->As<RE::TESEnchantableForm>() && a_object->As<RE::TESEnchantableForm>()->formEnchanting);
            if (enchanted)
                set(LootFilter::Category::e_enchanted);
        }

        // 高价值：单件价值 >= 阈值
        if (a_cfg.value_high_value && item_value(a_object, a_entry, a_count) >= static_cast<std::int32_t>(a_cfg.high_value_threshold))
            set(LootFilter::Category::e_valuable);

        // 书籍：0=全部书籍（含笔记/信件），1=法术+技能书，2=仅法术书
        if (a_cfg.value_books && type == RE::FormType::Book)
        {
            RE::TESObjectBOOK* const book = a_object->As<RE::TESObjectBOOK>();
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
                consumable = !a_cfg.soul_gem_filled_only || (a_entry && a_entry->GetSoulLevel() != RE::SOUL_LEVEL::kNone);
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

        // 运行时库存：死亡 Actor 的随身物品（含灰烬堆关联 Actor）与静态尸体的运行时变更。
        // a_noInit=true：只读，不创建 InventoryChanges。
        if (RE::InventoryChanges* changes = a_ref->GetInventoryChanges(true))
        {
            if (changes->entryList)
            {
                for (RE::InventoryEntryData* entry : *changes->entryList)
                {
                    // 注意：GetObject() 会被 windows.h 的 GetObject 宏展开（GetObjectA），改用公开成员
                    if (!entry || !entry->object)
                        continue;
                    RE::TESBoundObject* const object = entry->object;
                    std::int32_t const count = entry->countDelta > 0 ? entry->countDelta : 1;
                    result.categories |= classify_item(object, entry, count, cfg);
                    result.best_item_value = std::max(result.best_item_value, item_value(object, entry, count));
                }
            }
        }

        // 基础容器：静态尸体等 CONT 记录的默认战利品（无实例信息）
        if (RE::TESContainer const* container = a_ref->GetContainer())
        {
            container->ForEachContainerObject([&](RE::ContainerObject& a_entry) {
                RE::TESBoundObject* const object = a_entry.obj;
                if (!object)
                    return RE::BSContainer::ForEachResult::kContinue;
                result.categories |= classify_item(object, nullptr, a_entry.count, cfg);
                result.best_item_value = std::max(result.best_item_value, item_value(object, nullptr, a_entry.count));
                return RE::BSContainer::ForEachResult::kContinue;
            });
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
