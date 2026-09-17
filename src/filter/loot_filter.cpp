//
// Created by AmazingBuff on 2026/08/18.
//

#include "loot_filter.h"

#include "config/config.h"

#include "Plugin.h"

PLUGIN_NAMESPACE_BEGIN

LootFilter& LootFilter::instance()
{
    static LootFilter s_instance;
    return s_instance;
}

namespace
{
    [[nodiscard]] int32_t item_value(RE::InventoryEntryData const& a_entry, int32_t a_count)
    {
        RE::TESBoundObject* const object = a_entry.object;
        if (!object)
            return 0;
        if (object->IsGold())
            return a_count;
        return a_entry.GetValue();
    }

    [[nodiscard]] RE::stl::enumeration<LootFilter::Category> classify_item(RE::InventoryEntryData const& a_entry, int32_t a_count, Config const& a_cfg)
    {
        RE::stl::enumeration<LootFilter::Category> cats = LootFilter::Category::e_none;
        RE::TESBoundObject* const object = a_entry.object;
        if (!object)
            return cats;

        if (a_cfg.value_quest_items && a_entry.IsQuestObject())
            cats |= LootFilter::Category::e_quest;

        RE::FormType const type = object->GetFormType();

        if (a_cfg.value_keys && type == RE::FormType::KeyMaster)
            cats |= LootFilter::Category::e_key;

        if (a_cfg.value_enchanted && a_entry.IsEnchanted())
            cats |= LootFilter::Category::e_enchanted;

        if (a_cfg.value_high_value && item_value(a_entry, a_count) >= a_cfg.high_value_threshold)
            cats |= LootFilter::Category::e_valuable;

        if (a_cfg.book_filter_mode != Config::BookType::e_none && type == RE::FormType::Book)
        {
            if (RE::TESObjectBOOK const *const book = object->As<RE::TESObjectBOOK>())
            {
                bool match = true;
                if (a_cfg.book_filter_mode & Config::BookType::e_spell)
                    match |= book->TeachesSpell();
                if (a_cfg.book_filter_mode & Config::BookType::e_skill)
                    match |= book->TeachesSkill();
                if (a_cfg.book_filter_mode & Config::BookType::e_not_read)
                    match |= !book->IsRead();
                if (match)
                    cats |= LootFilter::Category::e_book;
            }
        }

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
                cats |= LootFilter::Category::e_consumable;
        }

        return cats;
    }
}

// fork from QuickLoot IE src/items/inventory.cpp
RE::BSTArray<RE::InventoryEntryData> LootFilter::fetch_inventory_items(RE::TESObjectREFR* a_ref, std::function<bool(RE::TESBoundObject&)> const& filter)
{
        RE::InventoryChanges* const changes = a_ref->GetInventoryChanges();

        if (RE::Actor* const actor = a_ref->As<RE::Actor>())
        {
            if (changes)
                m_refresh_enchanted_weapons(actor, changes);
        }

        std::unordered_map<RE::TESBoundObject*, RE::InventoryEntryData> lookup;

        // Changed items
        if (changes && changes->entryList)
        {
            for (RE::InventoryEntryData const* entry : *changes->entryList)
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
                if (object && filter(*object) && object->GetFormType() != RE::FormType::LeveledItem)
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
            for (RE::ObjectRefHandle const& drop_ref_handle : extra_drops->droppedItemList)
            {
                RE::NiPointer<RE::TESObjectREFR> const reference = drop_ref_handle.get();

                if (reference && !reference->IsDeleted() && !reference->IsDisabled())
                {
                    RE::TESBoundObject* const object = reference->GetObjectReference();
                    if (object && filter(*object))
                    {
                        int32_t const count = reference->extraList.GetCount();
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

LootFilter::EvaluateResult LootFilter::evaluate(RE::TESObjectREFR* a_ref)
{
    EvaluateResult result{
        .has_items = false,
        .categories = Category::e_none,
        .best_item_value = 0
    };
    if (!a_ref)
        return result;

    Config const& cfg = Setting::instance().get_config();

    for (RE::InventoryEntryData const& entry : fetch_inventory_items(a_ref, RE::TESObjectREFR::DEFAULT_INVENTORY_FILTER))
    {
        RE::TESBoundObject const* const object = entry.object;
        if (!object)
            continue;

        if (object->GetFormType() == RE::FormType::LeveledItem)
            continue;

        if (!object->GetPlayable())
            continue;

        result.has_items = true;
        result.categories |= classify_item(entry, entry.countDelta, cfg);
        result.best_item_value = std::max(result.best_item_value, item_value(entry, entry.countDelta));
    }

    if (cfg.value_filter_enabled && result.categories == Category::e_none)
        result.has_items = false;

    return result;
}

LootFilter::LootFilter() : m_refresh_enchanted_weapons{ RELOCATION_ID(50946, 51823) } {}

LootFilter::~LootFilter() = default;

PLUGIN_NAMESPACE_END