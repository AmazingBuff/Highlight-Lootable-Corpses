//
// Created by AmazingBuff on 2026/9/10.
//

#pragma once

#include "def.h"

#include "Plugin.h"

namespace RE
{
    class Actor;
    class TESObjectREFR;
}

PLUGIN_NAMESPACE_BEGIN

namespace Util
{
    [[nodiscard]] RE::TESObjectREFR* get_container_object(RE::TESObjectREFR* a_ref);

    [[nodiscard]] bool is_corpse_actor(RE::Actor* a_actor);
    [[nodiscard]] bool is_ash_pile(const RE::TESObjectREFR* a_ref);
    [[nodiscard]] bool is_corpse_object(const RE::TESObjectREFR* a_ref);
    [[nodiscard]] bool is_corpse(RE::TESObjectREFR* a_ref);
}
PLUGIN_NAMESPACE_END