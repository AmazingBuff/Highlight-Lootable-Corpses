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

        // 消耗品：箭矢/炼金材料/药水/卷轴/灵魂石
        if (a_cfg.value_consumables)
        {
            bool consumable = false;
            switch (type)
            {
            case RE::FormType::Ammo:
            case RE::FormType::Ingredient:
            case RE::FormType::AlchemyItem:
            case RE::FormType::Scroll:
            case RE::FormType::SoulGem:
                consumable = true;
                break;
            default:
                break;
            }
            if (consumable)
                set(LootFilter::Category::e_consumable);
        }

        return cats;
    }


    // fork from QuickLoot IE src/items/inventory.cpp
    using func_t = void (*)(RE::Actor*, RE::InventoryChanges*);
    REL::Relocation<func_t> g_refresh_enchanted_weapons{ RELOCATION_ID(50946, 51823) };

    RE::BSTArray<RE::InventoryEntryData> fetch_inventory_items(RE::TESObjectREFR* a_ref, const std::function<bool(RE::TESBoundObject&)>& filter)
    {
        RE::InventoryChanges* const changes = a_ref->GetInventoryChanges();

		if (RE::Actor* const actor = skyrim_cast<RE::Actor*>(a_ref))
		{
		    if (changes)
		        g_refresh_enchanted_weapons(actor, changes);
		}

		std::unordered_map<RE::TESBoundObject*, RE::InventoryEntryData> lookup;

		// Changed items
		if (changes && changes->entryList)
		{
			for (const RE::InventoryEntryData* entry : *changes->entryList)
			{
			    if (entry && entry->object && filter(*entry->object))
			    {
			        lookup.emplace(entry->object, *entry);
			    }
			}
		}

		// Base container items
		if (RE::TESContainer const* const container = a_ref->GetContainer())
		{
			container->ForEachContainerObject([&](RE::ContainerObject& entry)
			{
			    RE::TESBoundObject* const object = entry.obj;
                if (object && filter(*object) && !skyrim_cast<RE::TESLevItem*>(object))
                {
                    if (auto const it = lookup.find(object); it == lookup.end())
                        lookup.emplace(object, RE::InventoryEntryData{object, entry.count});
                    else
                    {
                        RE::InventoryEntryData& inventory_entry = it->second;
                        if (!inventory_entry.IsLeveled())
                            inventory_entry.countDelta += entry.count;
                    }
                }
			    return RE::BSContainer::ForEachResult::kContinue;
			});
		}

		// Dropped items always appear as separate item stacks because we need to attach the drop ref to them.
		if (RE::ExtraDroppedItemList* const extra_drops = a_ref->extraList.GetByType<RE::ExtraDroppedItemList>())
		{
			for (const RE::ObjectRefHandle& drop_ref_handle : extra_drops->droppedItemList) {
				const RE::NiPointer<RE::TESObjectREFR> reference = drop_ref_handle.get();

			    if (reference && !reference->IsDeleted() && !reference->IsDisabled())
			    {
			        RE::TESBoundObject* const object = reference->GetObjectReference();
			        if (object && filter(*object))
			        {
			            const int32_t count = reference->extraList.GetCount();
			            if (auto const it = lookup.find(object); it == lookup.end())
			                lookup.emplace(object, RE::InventoryEntryData{object, count});
			            else
			            {
			                RE::InventoryEntryData& inventory_entry = it->second;
			                if (!inventory_entry.IsLeveled())
			                    inventory_entry.countDelta += count;
			            }
			        }
			    }
			}
		}

        RE::BSTArray<RE::InventoryEntryData> inventory;
        for (auto const& entry : lookup | std::views::values)
        {
            if (entry.countDelta > 0)
                inventory.emplace_back(entry);
        }

		return inventory;
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

        for (RE::InventoryEntryData const& entry : fetch_inventory_items(a_ref, RE::TESObjectREFR::DEFAULT_INVENTORY_FILTER))
        {
            RE::TESBoundObject const* const object = entry.object;
            if (!object)
                continue;

            if (object->GetFormType() == RE::FormType::LeveledItem)
                continue;  // 引擎仍未 resolve 的等级条目：跳过（QuickLoot IE 同款取舍）

            if (!object->GetPlayable())
                continue;  // 玩家不可拿取的残留物不算"还有货"

            result.has_items = true;
            result.categories |= classify_item(entry, entry.countDelta, cfg);
            result.best_item_value = std::max(result.best_item_value, item_value(entry, entry.countDelta));
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

    std::uint64_t config_stamp(Config::Settings const& a_cfg)
    {
        std::uint64_t stamp = 0;
        auto const bit = [&](bool a_value, std::uint64_t a_shift) {
            if (a_value)
                stamp |= std::uint64_t{ 1 } << a_shift;
        };
        bit(a_cfg.value_quest_items, 0);
        bit(a_cfg.value_keys, 1);
        bit(a_cfg.value_enchanted, 2);
        bit(a_cfg.value_high_value, 3);
        bit(a_cfg.value_books, 4);
        bit(a_cfg.value_consumables, 5);
        stamp |= static_cast<std::uint64_t>(static_cast<std::uint32_t>(a_cfg.book_filter_mode) & 0x3) << 8;

        // 高价值阈值按 IEEE 位模式编码（任何位模式都可可靠比较）
        std::uint32_t threshold_bits = 0;
        static_assert(sizeof(threshold_bits) == sizeof(a_cfg.high_value_threshold));
        std::memcpy(&threshold_bits, &a_cfg.high_value_threshold, sizeof(threshold_bits));
        stamp |= static_cast<std::uint64_t>(threshold_bits) << 32;
        return stamp;
    }
}
