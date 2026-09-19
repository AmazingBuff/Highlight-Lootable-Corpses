//
// Created by AmazingBuff on 2026/09/09.
//

#include "searched_corpses.h"

#include "base/def.h"
#include "base/util.h"

PLUGIN_NAMESPACE_BEGIN

namespace
{
    constexpr uint32_t Record_ID = static_cast<uint32_t>(hash_str(Plugin::Plugin_Name.data(), Plugin::Plugin_Name.size(), Amazing_Hash));
    constexpr uint32_t Record_Version = Plugin::Plugin_Version[0];

    class ActivateHandler final : public RE::BSTEventSink<RE::TESActivateEvent>
    {
    public:
        static ActivateHandler* instance()
        {
            static ActivateHandler s_instance;
            return &s_instance;
        }

        RE::BSEventNotifyControl ProcessEvent(
            RE::TESActivateEvent const* event,
            [[maybe_unused]] RE::BSTEventSource<RE::TESActivateEvent>* source) override
        {
            if (event && event->objectActivated)
            {
                auto const* action = event->actionRef ? event->actionRef->As<RE::Actor>() : nullptr;
                if (action && action->IsPlayerRef())
                {
                    RE::TESObjectREFR* const object = event->objectActivated.get();
                    if (Util::is_corpse(object))
                        MarkCorpse::instance().mark(Util::get_container_object(object));
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
}

MarkCorpse& MarkCorpse::instance()
{
    static MarkCorpse s_instance;
    return s_instance;
}

void MarkCorpse::mark(RE::TESObjectREFR* ref)
{
    if (ref)
    {
        RE::FormID const form_id = ref->GetFormID();
        if (!m_searched_corpses.contains(form_id))
        {
            m_searched_corpses.insert(form_id);
            logger::info("{} ({:08x}) has been removed!", ref->GetDisplayFullName(), form_id);
        }
    }
}

bool MarkCorpse::contains(RE::TESObjectREFR* ref) const
{
    if (!ref || m_searched_corpses.empty())
        return false;

    if (m_searched_corpses.contains(ref->GetFormID()))
        return true;
    return false;
}

void MarkCorpse::install()
{
    SKSE::SerializationInterface const* serialization = SKSE::GetSerializationInterface();
    if (!serialization)
    {
        logger::error("Serialization interface unavailable, searched-corpses persistence disabled"sv);
        return;
    }

    serialization->SetUniqueID(Record_ID);

    serialization->SetSaveCallback([](SKSE::SerializationInterface* intfc)
    {
        uint32_t const count = static_cast<uint32_t>(instance().m_searched_corpses.size());
        if (!intfc->WriteRecord(Record_ID, Record_Version, &count, sizeof(count)))
        {
            logger::error("Failed to write searched-corpses record header"sv);
            return;
        }
        for (RE::FormID const& id : instance().m_searched_corpses)
        {
            if (!intfc->WriteRecordData(&id, sizeof(RE::FormID)))
            {
                logger::error("Failed to write searched-corpses entry {:08X}"sv, id);
                return;
            }
        }
        logger::info("Saved {} searched-corpses marks"sv, count);
    });

    serialization->SetLoadCallback([](SKSE::SerializationInterface* intfc)
    {
        instance().m_searched_corpses.clear();

        uint32_t type = 0;
        uint32_t version = 0;
        uint32_t length = 0;
        while (intfc->GetNextRecordInfo(type, version, length))
        {
            if (type != Record_ID)
                continue;

            if (version != Record_Version)
            {
                logger::warn("Unsupported searched-corpses record version {}, skipping"sv, version);
                continue;
            }

            uint32_t count = 0;
            if (length < sizeof(uint32_t) || !intfc->ReadRecordData(count))
            {
                logger::error("Corrupt searched-corpses record header"sv);
                continue;
            }

            uint32_t kept = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                RE::FormID stored = 0;
                if (!intfc->ReadRecordData(stored))
                {
                    logger::error("Corrupt searched-corpses entry #{}/{}"sv, i, count);
                    break;
                }
                RE::FormID resolved = 0;
                if (intfc->ResolveFormID(stored, resolved))
                {
                    instance().m_searched_corpses.insert(resolved);
                    ++kept;
                }
            }
            logger::info("Loaded {} searched-corpses marks ({} dropped as stale)"sv, kept, count - kept);
        }
    });

    serialization->SetRevertCallback([]([[maybe_unused]] SKSE::SerializationInterface* intfc)
    {
        instance().m_searched_corpses.clear();
    });

    serialization->SetFormDeleteCallback([](RE::VMHandle handle)
    {
        instance().m_searched_corpses.erase(static_cast<RE::FormID>(handle & 0xFFFFFFFFu));
    });

    RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink(ActivateHandler::instance());

    logger::info("Registered searched corpses serialization callbacks and activate event sink"sv);
}

MarkCorpse::MarkCorpse() = default;

MarkCorpse::~MarkCorpse() = default;

PLUGIN_NAMESPACE_END