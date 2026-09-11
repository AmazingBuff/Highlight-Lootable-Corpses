//
// Created by AmazingBuff on 2026/08/18.
//

#pragma once

#include <cstdint>
#include <string>


PLUGIN_NAMESPACE_BEGIN

class LootFilter
{
public:
    LootFilter() = delete;
    ~LootFilter() = delete;
    LootFilter(LootFilter const&) = delete;
    LootFilter(LootFilter const&&) = delete;
    LootFilter operator=(LootFilter&) = delete;
    LootFilter operator=(LootFilter&&) = delete;

    enum class Category : std::uint16_t
    {
        e_none              = 0,
        e_quest             = 1 << 0,
        e_key               = 1 << 1,
        e_enchanted         = 1 << 2,
        e_valuable          = 1 << 3,
        e_book              = 1 << 4,
        e_consumable        = 1 << 5,

        e_all               = 0xFFFF,
    };

    struct EvaluateResult
    {
        bool has_items;
        RE::stl::enumeration<Category> categories;
        std::int32_t best_item_value;
    };

    // input must be a container
    static EvaluateResult evaluate(RE::TESObjectREFR* a_ref);
};

PLUGIN_NAMESPACE_END