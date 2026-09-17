//
// Created by AmazingBuff on 2026/09/09.
//

#include "quickloot_compat.h"

#include "QuickLootIE/v3_4/QuickLootAPI.h"
#include "QuickLootIE/v4_0/QuickLootAPI.h"
#include "base/util.h"
#include "search/searched_corpses.h"

#include "Plugin.h"

PLUGIN_NAMESPACE_BEGIN

namespace
{
    void OnOpeningLootMenu40(QuickLoot::API::OpeningLootMenuEvent* a_event)
    {
        if (!a_event || !a_event->container)
            return;

        if (RE::NiPointer<RE::TESObjectREFR> const ref = a_event->container.get())
        {
          if (RE::TESObjectREFR* r = ref.get(); Util::is_corpse(r))
                MarkCorpse::instance().mark(Util::get_container_object(r));
        }
    }

    void OnOpeningLootMenu34(QuickLoot::OpeningLootMenuEvent* a_event)
    {
        if (a_event && Util::is_corpse(a_event->container))
            MarkCorpse::instance().mark(Util::get_container_object(a_event->container));
    }


    bool install_v34()
    {
        QuickLoot::QuickLootAPI::Init();
        if (!QuickLoot::QuickLootAPI::IsReady())
        {
            logger::info("QuickLoot IE 3.4 API unreachable, searched-corpses marks rely on activation events only"sv);
            return false;
        }

        if (!QuickLoot::QuickLootAPI::RegisterOpeningLootMenuHandler(&OnOpeningLootMenu34))
        {
            logger::info("QuickLoot IE 3.4 rejected OpeningLootMenu registration, marks rely on activation events only"sv);
            return false;
        }

        logger::info("QuickLoot IE (3.4 PluginRequests) detected: loot menu openings now mark corpses as searched"sv);
        return true;
    }

    bool install_v40()
    {
        if (!QuickLoot::API::QuickLootAPI::Init(Plugin::Plugin_Name.data()))
        {
            logger::info("Error while initializing QLIE API");
            return false;
        }
        QuickLoot::API::QuickLootAPI::RegisterOpeningLootMenuHandler(&OnOpeningLootMenu40);

        logger::info("QuickLoot IE detected (4.0 interface): loot menu openings now mark corpses as searched"sv);
        return true;
    }
}

bool QuickLootCompat::install()
{
    if (install_v40())
        return true;

    return install_v34();
}

PLUGIN_NAMESPACE_END