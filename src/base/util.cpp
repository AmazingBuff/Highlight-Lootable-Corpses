//
// Created by AmazingBuff on 2026/9/10.
//

#include "util.h"

#include "Plugin.h"

#include <cstdint>
#include <string_view>

namespace
{
    constexpr std::string_view Skyrim_Plugin = std::string_view{ "Skyrim.esm" };
    constexpr std::string_view Dawnguard_Plugin = std::string_view{ "Dawnguard.esm" };
    constexpr std::string_view Dragonborn_Plugin = std::string_view{ "Dragonborn.esm" };

    struct LocalFromID
    {
        uint32_t local_id;
        std::string_view plugin_name;
    };

    std::vector<LocalFromID> const Ash_Piles =
    {
        { .local_id = 0x0000001B, .plugin_name = Skyrim_Plugin }, // DefaultAshPile1 = 0x1B
        { .local_id = 0x00000022, .plugin_name = Skyrim_Plugin }, // DefaultAshPile2 = 0x22
        { .local_id = 0x00101048, .plugin_name = Skyrim_Plugin }, // Ghost = 0x101048
        { .local_id = 0x001069E4, .plugin_name = Skyrim_Plugin }, // Ice = 0x1069E4
        { .local_id = 0x0010C649, .plugin_name = Skyrim_Plugin }, // DarkGhost = 0x10C649
        { .local_id = 0x0010D6EF, .plugin_name = Skyrim_Plugin }, // GhostBlack = 0x10D6EF
        { .local_id = 0x0000A905, .plugin_name = Dawnguard_Plugin },  // DLC01DefaultAshPileSoul = 0xA905
        { .local_id = 0x0000BDCE, .plugin_name = Dawnguard_Plugin },  // DLC01DefaultAshPileEnemies = 0xBDCE
        { .local_id = 0x0000FC74, .plugin_name = Dawnguard_Plugin },  // DLC1dunHarkonAshPile = 0xFC74
        { .local_id = 0x00003522, .plugin_name = Dawnguard_Plugin },  // DLC1_WESC08AshPile = 0x3522
        { .local_id = 0x0003280A, .plugin_name = Dragonborn_Plugin }, // DLC2AshSpawnAshPile = 0x3280A
        { .local_id = 0x00023F83, .plugin_name = Dragonborn_Plugin }, // DLC2HMDaedraAshPile = 0x23F83
   };

    std::vector<LocalFromID> const Static_Corpses =
    {
        { .local_id = 0x00023969, .plugin_name = Skyrim_Plugin },  // TreasDraugrAmbushCorpse01
        { .local_id = 0x0008008D, .plugin_name = Skyrim_Plugin },  // TreasDraugrAmbushCorpseWrapped01
        { .local_id = 0x0008008E, .plugin_name = Skyrim_Plugin },  // TreasDraugrAmbushCorpseWrapped02
        { .local_id = 0x0008008F, .plugin_name = Skyrim_Plugin },  // TreasDraugrAmbushCorpse02
        { .local_id = 0x00080090, .plugin_name = Skyrim_Plugin },  // TreasDraugrAmbushCorpse03
        { .local_id = 0x00080091, .plugin_name = Skyrim_Plugin },  // TreasDraugrAmbushCorpse04
        { .local_id = 0x00080092, .plugin_name = Skyrim_Plugin },  // TreasDraugrAmbushCorpse05
        { .local_id = 0x00080093, .plugin_name = Skyrim_Plugin },  // TreasDraugrAmbushCorpse06
        { .local_id = 0x00080094, .plugin_name = Skyrim_Plugin },  // TreasDraugrAmbushCorpse07
        { .local_id = 0x00042745, .plugin_name = Skyrim_Plugin },  // TreasBurntCorpse01
        { .local_id = 0x00042746, .plugin_name = Skyrim_Plugin },  // TreasBurntCorpse02
        { .local_id = 0x00042747, .plugin_name = Skyrim_Plugin },  // TreasBurntCorpse03
        { .local_id = 0x00042748, .plugin_name = Skyrim_Plugin },  // TreasBurntCorpse04
        { .local_id = 0x00042749, .plugin_name = Skyrim_Plugin },  // TreasBurntCorpse05
        { .local_id = 0x000DD060, .plugin_name = Skyrim_Plugin },  // MQ104BurntCorpse03
        { .local_id = 0x000DD061, .plugin_name = Skyrim_Plugin },  // MQ104BurntCorpse04
        { .local_id = 0x000BAD05, .plugin_name = Skyrim_Plugin },  // TreasCorpseMammoth
        { .local_id = 0x000D4FFD, .plugin_name = Skyrim_Plugin },  // POICorpseFrozenMammoth
        { .local_id = 0x00020668, .plugin_name = Skyrim_Plugin },  // TreasSpiderWebCorpseHuman
        { .local_id = 0x000C674B, .plugin_name = Skyrim_Plugin },  // defaultGhostCorpse
        { .local_id = 0x000E7A36, .plugin_name = Skyrim_Plugin },  // dunGeirmundCorpse
        { .local_id = 0x00018E73, .plugin_name = Skyrim_Plugin },  // MS05_SvaknirsCorpse
        { .local_id = 0x0010EB29, .plugin_name = Skyrim_Plugin },  // wispCorpseContainer
        { .local_id = 0x00023968, .plugin_name = Skyrim_Plugin },  // DraugrBodyLaying0000 (STAT)
        // DLC
        { .local_id = 0x0000A904, .plugin_name = Dawnguard_Plugin },   // DLC01defaultSoulCorpse
        { .local_id = 0x00018C3B, .plugin_name = Dragonborn_Plugin },  // DLC2TreasDraugrAmbushCorpseWrapped01EMPTY
    };


    [[nodiscard]] std::vector<RE::FormID> resolve_form_ids(std::vector<LocalFromID> const& a_forms)
    {
        std::vector<RE::FormID> results;
        if (RE::TESDataHandler* data_handler = RE::TESDataHandler::GetSingleton())
        {
            results.reserve(a_forms.size());
            for (auto const& [local_id, plugin_name] : a_forms)
            {
                if (RE::FormID const id = data_handler->LookupFormID(local_id, plugin_name))
                    results.push_back(id);
            }
        }

        if (results.empty())
            logger::warn("Resolved 0 of {} static form IDs, related corpse detection is disabled", a_forms.size());

        return results;
    }

    [[nodiscard]] bool is_ref_form_in(RE::TESObjectREFR const* a_ref, std::vector<RE::FormID> const& a_ids)
    {
        if (!a_ref || a_ids.empty())
            return false;

        RE::TESBoundObject const* base = a_ref->GetBaseObject();
        if (!base)
            return false;

        RE::FormID const id = base->GetFormID();
        return std::ranges::any_of(a_ids, [id](RE::FormID const& a_form) { return id == a_form; });
    }
}


PLUGIN_NAMESPACE_BEGIN

namespace Util
{
    // fork from QuickLoot IE
    RE::TESObjectREFR* get_container_object(RE::TESObjectREFR* a_ref)
    {
        if (a_ref)
        {
            RE::TESBoundObject const* object = a_ref->GetObjectReference();

            // For enemies that leave behind an ash pile on death
            if (object->Is(RE::FormType::Activator))
            {
                RE::ObjectRefHandle ref_handle = a_ref->extraList.GetAshPileRef();
                if (RE::TESObjectREFRPtr const ptr = ref_handle.get())
                    return get_container_object(ptr.get());
            }

            if (a_ref->HasContainer())
                return a_ref;
        }

        return nullptr;
    }

    bool is_corpse_actor(RE::Actor* a_actor)
    {
        return a_actor->AsActorState()->GetLifeState() == RE::ACTOR_LIFE_STATE::kDead;
    }

    bool is_ash_pile(RE::TESObjectREFR const* a_ref)
    {
        static std::vector<RE::FormID> const s_ash_pile_ids = resolve_form_ids(Ash_Piles);
        return is_ref_form_in(a_ref, s_ash_pile_ids);
    }

    bool is_corpse_object(RE::TESObjectREFR const* a_ref)
    {
        static std::vector<RE::FormID> const s_static_corpses_ids = resolve_form_ids(Static_Corpses);
        return is_ref_form_in(a_ref, s_static_corpses_ids);
    }

    bool is_corpse(RE::TESObjectREFR* a_ref)
    {
        if (RE::Actor* const actor = a_ref->As<RE::Actor>())
            return is_corpse_actor(actor);
        return is_ash_pile(a_ref) || is_corpse_object(a_ref);
    }
}

PLUGIN_NAMESPACE_END