//
// Created by AmazingBuff on 2026/09/09.
//

#pragma once

#include "pch.h"

namespace SearchedCorpses
{
    // 已搜索尸体标记：玩家激活（搜索）过的尸体 FormID 集合——即使一件都没拿也
    // 标记，HideSearchedEnabled 开启时 scan() 不再为其显示 box。
    // 标记忠实于激活事实：拿空/塞回物品都不改写标记（box 显示由 has_items 与
    // 标记共同决定，两条件各管各的）；标记只随"引用不存在"而清除。

    // 玩家激活了 a_ref（已由事件侧确认为玩家行为）：标记 a_ref 及其灰烬堆
    // 关联 Actor（激活事件与扫描可能分别落在堆/Actor 两个引用上）
    void mark_activated(RE::TESObjectREFR* a_ref);

    // a_ref（或其灰烬堆关联 Actor）是否被搜索过
    [[nodiscard]] bool contains(RE::TESObjectREFR* a_ref);

    // 序列化（co-save）回调，在 SKSEPlugin_Load 中注册：
    //   Save：集合写入 co-save（随玩家存档持久化，按存档隔离）
    //   Load：读回集合，FormID 经 ResolveFormID 重映射，resolve 失败即丢弃
    //   Revert：清空集合（回主菜单/读档前，旧存档标记不残留）
    //   FormDelete：引擎删除 Form 时剔除对应标记（引用不存在 → 标记失去意义）
    void register_serialization_callbacks();

    // 读档/新游戏时清空运行时标记（kDataLoaded/kNewGame/kPostLoadGame 路径，
    // 与 Revert 互为兜底）
    void clear();
}
