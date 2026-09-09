//
// Created by AmazingBuff on 2026/09/09.
//

#include "pch.h"
#include "searched_corpses.h"

#include "corpse_finder.h"

#include <unordered_set>

namespace
{
    // 已搜索尸体 FormID 集合。激活事件（TESActivateEvent）、scan（游戏线程任务）、
    // 序列化回调（Save/Load/Revert/FormDelete）都在游戏线程 → 无需加锁。
    // 引擎对已搜索尸体没有原生标志，co-save 是唯一跨会话持久化途径。
    std::unordered_set<RE::FormID> g_searched_corpses;

    // 序列化 record 标识：4 字节需全环境唯一（社区惯例用插件名缩写）。
    // HLCS = Highlight Lootable Corpses, Searched
    constexpr std::uint32_t kRecordID = 'HLCS';
    constexpr std::uint32_t kRecordVersion = 1;

    void insert_with_ash_owner(RE::TESObjectREFR* a_ref)
    {
        g_searched_corpses.insert(a_ref->GetFormID());
        if (RE::Actor* const owner = CorpseFinder::find_ash_pile_owner(a_ref))
            g_searched_corpses.insert(owner->GetFormID());
    }
}

namespace SearchedCorpses
{
    void mark_activated(RE::TESObjectREFR* a_ref)
    {
        if (a_ref)
            insert_with_ash_owner(a_ref);
    }

    bool contains(RE::TESObjectREFR* a_ref)
    {
        if (!a_ref || g_searched_corpses.empty())
            return false;

        // 双向查询：本引用被搜索过，或其灰烬堆关联的原始 Actor 被搜索过
        if (g_searched_corpses.contains(a_ref->GetFormID()))
            return true;
        if (RE::Actor* const owner = CorpseFinder::find_ash_pile_owner(a_ref))
            return g_searched_corpses.contains(owner->GetFormID());
        return false;
    }

    void register_serialization_callbacks()
    {
        const auto serialization = SKSE::GetSerializationInterface();
        if (!serialization)
        {
            logger::error("Serialization interface unavailable, searched-corpses persistence disabled"sv);
            return;
        }

        serialization->SetUniqueID(kRecordID);

        serialization->SetSaveCallback([](SKSE::SerializationInterface* a_intfc) {
            // 头部：count + FormID×N。只存"活的"标记——集合已由 FormDelete 回调
            // 与读档 resolve 持续清理（见下），体量以"搜过但引用仍存在"为上界
            std::uint32_t const count = static_cast<std::uint32_t>(g_searched_corpses.size());
            if (!a_intfc->WriteRecord(kRecordID, kRecordVersion, &count, sizeof(count)))
            {
                logger::error("Failed to write searched-corpses record header"sv);
                return;
            }
            for (RE::FormID const id : g_searched_corpses)
            {
                if (!a_intfc->WriteRecordData(&id, sizeof(id)))
                {
                    logger::error("Failed to write searched-corpses entry {:08X}"sv, id);
                    return;
                }
            }
            logger::info("Saved {} searched-corpses marks"sv, count);
        });

        serialization->SetLoadCallback([](SKSE::SerializationInterface* a_intfc) {
            g_searched_corpses.clear();

            std::uint32_t type = 0;
            std::uint32_t version = 0;
            std::uint32_t length = 0;
            while (a_intfc->GetNextRecordInfo(type, version, length))
            {
                if (type != kRecordID)
                    continue;

                // 版本兼容：未来扩展格式时按 version 分支读取
                if (version != kRecordVersion)
                {
                    logger::warn("Unsupported searched-corpses record version {}, skipping"sv, version);
                    continue;
                }

                std::uint32_t count = 0;
                if (length < sizeof(count) || !a_intfc->ReadRecordData(count))
                {
                    logger::error("Corrupt searched-corpses record header"sv);
                    continue;
                }

                // 读档后 mod 加载顺序可能变化 → FormID 会漂移：
                // ResolveFormID 把存档时的 ID 重映射到本会话的 ID；
                // resolve 失败 = 引用已不存在（幽灵标记），丢弃
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

        serialization->SetRevertCallback([]([[maybe_unused]] SKSE::SerializationInterface* a_intfc) {
            // 回主菜单/读档前：清空旧存档标记（与 kDataLoaded 路径的 clear 互为兜底）
            g_searched_corpses.clear();
        });

        serialization->SetFormDeleteCallback([](RE::VMHandle a_handle) {
            // 引擎删除 Form（尸体腐烂清理/脚本删除等）→ 标记失去意义，剔除。
            // VMHandle 低 32 位即 FormID（未掩码值）
            g_searched_corpses.erase(static_cast<RE::FormID>(a_handle & 0xFFFFFFFFu));
        });

        logger::info("Registered searched-corpses serialization callbacks"sv);
    }

    void clear()
    {
        g_searched_corpses.clear();
    }
}
