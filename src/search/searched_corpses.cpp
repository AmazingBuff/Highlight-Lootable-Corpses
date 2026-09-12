//
// Created by AmazingBuff on 2026/09/09.
//

#include "searched_corpses.h"
#include "base/util.h"

PLUGIN_NAMESPACE_BEGIN

namespace
{
    constexpr std::uint32_t Record_ID = static_cast<uint32_t>(hash_str(Plugin::Plugin_Name.data(), Plugin::Plugin_Name.size(), Amazing_Hash));
    constexpr std::uint32_t Record_Version = Plugin::Plugin_Version[0];

    class ActivateHandler final : public RE::BSTEventSink<RE::TESActivateEvent>
    {
    public:
        static ActivateHandler* get_singleton()
        {
            static ActivateHandler instance;
            return &instance;
        }

        RE::BSEventNotifyControl ProcessEvent(
            RE::TESActivateEvent const* a_event,
            [[maybe_unused]] RE::BSTEventSource<RE::TESActivateEvent>* a_source) override
        {
            if (a_event && a_event->objectActivated)
            {
                auto const* action = a_event->actionRef ? a_event->actionRef->As<RE::Actor>() : nullptr;
                if (action && action->IsPlayerRef())
                {
                    RE::TESObjectREFR* const object = a_event->objectActivated.get();
                    if (Util::is_corpse(object))
                        MarkCorpse::mark(Util::get_container_object(object));
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    std::unordered_set<RE::FormID> g_searched_corpses;
}

void MarkCorpse::mark(RE::TESObjectREFR* a_ref)
{
    if (a_ref)
        g_searched_corpses.insert(a_ref->GetFormID());
}

bool MarkCorpse::contains(RE::TESObjectREFR* a_ref)
{
    if (!a_ref || g_searched_corpses.empty())
        return false;

    if (g_searched_corpses.contains(a_ref->GetFormID()))
        return true;
    return false;
}

void MarkCorpse::install()
{
    const SKSE::SerializationInterface* serialization = SKSE::GetSerializationInterface();
    if (!serialization)
    {
        logger::error("Serialization interface unavailable, searched-corpses persistence disabled"sv);
        return;
    }

    serialization->SetUniqueID(Record_ID);

    serialization->SetSaveCallback([](SKSE::SerializationInterface* a_intfc)
    {
        std::uint32_t const count = static_cast<std::uint32_t>(g_searched_corpses.size());
        if (!a_intfc->WriteRecord(Record_ID, Record_Version, &count, sizeof(count)))
        {
            logger::error("Failed to write searched-corpses record header"sv);
            return;
        }
        for (RE::FormID const& id : g_searched_corpses)
        {
            if (!a_intfc->WriteRecordData(&id, sizeof(RE::FormID)))
            {
                logger::error("Failed to write searched-corpses entry {:08X}"sv, id);
                return;
            }
        }
        logger::info("Saved {} searched-corpses marks"sv, count);
    });

    serialization->SetLoadCallback([](SKSE::SerializationInterface* a_intfc)
    {
        g_searched_corpses.clear();

        std::uint32_t type = 0;
        std::uint32_t version = 0;
        std::uint32_t length = 0;
        while (a_intfc->GetNextRecordInfo(type, version, length))
        {
            if (type != Record_ID)
                continue;

            if (version != Record_Version)
            {
                logger::warn("Unsupported searched-corpses record version {}, skipping"sv, version);
                continue;
            }

            std::uint32_t count = 0;
            if (length < sizeof(std::uint32_t) || !a_intfc->ReadRecordData(count))
            {
                logger::error("Corrupt searched-corpses record header"sv);
                continue;
            }

            std::uint32_t kept = 0;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                RE::FormID stored = 0;
                if (!a_intfc->ReadRecordData(stored))
                {
                    logger::error("Corrupt searched-corpses entry #{}/{}"sv, i, count);
                    break;
                }
                RE::FormID resolved = 0;
                if (a_intfc->ResolveFormID(stored, resolved))
                {
                    g_searched_corpses.insert(resolved);
                    ++kept;
                }
            }
            logger::info("Loaded {} searched-corpses marks ({} dropped as stale)"sv, kept, count - kept);
        }
    });

    serialization->SetRevertCallback([]([[maybe_unused]] SKSE::SerializationInterface* a_intfc)
    {
        g_searched_corpses.clear();
    });

    serialization->SetFormDeleteCallback([](RE::VMHandle a_handle)
    {
        g_searched_corpses.erase(static_cast<RE::FormID>(a_handle & 0xFFFFFFFFu));
    });

    logger::info("Registered searched-corpses serialization callbacks"sv);


    RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink(ActivateHandler::get_singleton());
    logger::info("Installed TESActivateEvent sinks"sv);
}

PLUGIN_NAMESPACE_END