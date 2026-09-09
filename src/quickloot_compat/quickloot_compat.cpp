//
// Created by AmazingBuff on 2026/09/09.
//

#include "pch.h"
#include "quickloot_compat.h"

#include "searched_corpses.h"

// QuickLoot IE 的公开集成头（MIT 许可，header-only，vendored 于本目录）：
// 内部经 GetModuleHandleA("QuickLootIE") + GetProcAddress 动态解析，DLL 不存在
// 时 Init 返回 false——可选依赖，无链接期要求。
#pragma warning(push)
#include "QuickLootAPI.h"
#pragma warning(pop)

namespace
{
    // OpeningLootMenuEvent：QLIE 弹出战利品菜单前同步派发（LootMenuManager::
    // RequestShow），container 即玩家正对的尸体——"看过里面有什么"就是搜索。
    // QLIE 的拿取路径（Take/Take All → RemoveItem）不产生 TESActivateEvent，
    // 此 handler 是 QLIE 用户标记"已搜索"的唯一入口。
    void OnOpeningLootMenu(QuickLoot::API::OpeningLootMenuEvent* a_event)
    {
        if (!a_event || !a_event->container)
            return;

        if (RE::NiPointer<RE::TESObjectREFR> const ref = a_event->container.get())
            SearchedCorpses::mark_activated(ref.get());

        // 不拦截：result 保持 kContinue，菜单正常打开
    }
}

namespace QuickLootCompat
{
    bool install()
    {
        // Init：GetModuleHandleA("QuickLootIE") 探测 + GetProcAddress 取接口。
        // 未安装 QLIE 时返回 false——标记退回纯 TESActivateEvent 路径，零影响
        if (!QuickLoot::API::QuickLootAPI::Init(Plugin::NAME.data())) {
            logger::info("QuickLoot IE not installed, searched-corpses marks rely on activation events only"sv);
            return false;
        }

        QuickLoot::API::QuickLootAPI::RegisterOpeningLootMenuHandler(&OnOpeningLootMenu);
        logger::info("QuickLoot IE detected: loot menu openings now mark corpses as searched"sv);
        return true;
    }
}
