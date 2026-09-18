//
// Created by AmazingBuff on 2026/09/09.
//

#pragma once

PLUGIN_NAMESPACE_BEGIN

class QuickLootCompat
{
public:
    QuickLootCompat() = delete;
    ~QuickLootCompat() = delete;
    QuickLootCompat(QuickLootCompat const&) = delete;
    QuickLootCompat(QuickLootCompat const&&) = delete;
    QuickLootCompat operator=(QuickLootCompat&) = delete;
    QuickLootCompat operator=(QuickLootCompat&&) = delete;

    [[nodiscard]] static bool install();
};

PLUGIN_NAMESPACE_END