//
// Created by AmazingBuff on 2026/09/09.
//

#include "pch.h"
#include "quickloot_compat.h"

#include "corpse_finder.h"
#include "searched_corpses.h"

#include <optional>

// QuickLoot IE 的公开集成头（MIT 许可，header-only，vendored 于本目录）：
// 内部经 GetModuleHandleA("QuickLootIE") + GetProcAddress 动态解析，DLL 不存在
// 时 Init 返回 false——可选依赖，无链接期要求。
#include "QuickLootAPI.h"

namespace
{
    // OpeningLootMenuEvent：QLIE 弹出战利品菜单前同步派发（LootMenuManager::
    // RequestShow），container 即玩家正对的引用——"看过里面有什么"就是搜索。
    // QLIE 的拿取路径（Take/Take All → RemoveItem）不产生 TESActivateEvent，
    // 此 handler 是 QLIE 用户标记"已搜索"的唯一入口。
    // 只标记尸体类引用（与 scan/激活事件侧口径一致），普通容器（宝箱/桶等）
    // 不进集合——它们本就不在 scan 的候选范围里，标记了也永远不会被查询
    void OnOpeningLootMenu(QuickLoot::API::OpeningLootMenuEvent* a_event)
    {
        if (!a_event || !a_event->container)
            return;

        if (RE::NiPointer<RE::TESObjectREFR> const ref = a_event->container.get())
        {
            RE::TESObjectREFR* const object = ref.get();
            bool is_corpse = false;
            if (RE::Actor* const actor = object->As<RE::Actor>())
                is_corpse = CorpseFinder::is_dead_corpse_actor(actor);
            else
                is_corpse = CorpseFinder::is_ash_pile_ref(object) || CorpseFinder::is_corpse_object_ref(object);
            if (is_corpse)
                SearchedCorpses::mark_activated(object);
        }

        // 不拦截：result 保持 kContinue，菜单正常打开
    }
}

namespace QuickLootCompat
{
    bool install()
    {
        // 幂等：消息回调可能多次进入（如误把本函数挂到周期性消息上），已注册
        // 则直接返回上次结果，不重复向 QLIE push handler
        static std::optional<bool> const installed = [] {
            // Init：GetModuleHandleA("QuickLootIE") 探测 + GetProcAddress 取接口。
            // 调用时机必须晚于所有插件的 SKSEPlugin_Load（kPostLoad 消息）——
            // SKSE 逐个加载插件且顺序按文件名字母序（H 先于 Q），在自家 Load 里
            // 探测时 QuickLootIE.dll 必然尚未加载，GetModuleHandle 只会拿到 NULL。
            // 未安装 QLIE 时 Init 返回 false——标记退回纯 TESActivateEvent 路径
            if (!QuickLoot::API::QuickLootAPI::Init(Plugin::NAME.data())) {
                logger::info("QuickLoot IE not installed, searched-corpses marks rely on activation events only"sv);
                return false;
            }

            // 只用 V20 的 RegisterOpeningLootMenuHandler：按 kV20 探测即可兼容
            // 尚无 V21 接口的旧版 QLIE（kLatest 会无谓拒绝它们）
            if (!QuickLoot::API::QuickLootAPI::IsReady(QuickLoot::API::ApiVersion::kV20)) {
                logger::info("QuickLoot IE API v20 unavailable, searched-corpses marks rely on activation events only"sv);
                return false;
            }

            QuickLoot::API::QuickLootAPI::RegisterOpeningLootMenuHandler(&OnOpeningLootMenu);
            logger::info("QuickLoot IE detected: loot menu openings now mark corpses as searched"sv);
            return true;
        }();
        return *installed;
    }
}
