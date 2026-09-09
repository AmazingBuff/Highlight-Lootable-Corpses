//
// Created by AmazingBuff on 2026/09/09.
//

#pragma once

namespace QuickLootCompat
{
    // QuickLoot IE 兼容层（可选依赖）：QLIE 的拿取路径走 RemoveItem，不产生
    // TESActivateEvent——只靠激活事件标记"已搜索"的话，QLIE 用户永远触发不了。
    // 本模块通过 QLIE 公开 API（header-only，GetProcAddress 动态解析，缺 DLL
    // 时优雅降级为纯激活事件标记）把"打开战利品菜单"也计入搜索标记。

    // 探测 QuickLootIE.dll 并注册 OpeningLootMenu handler。
    // 在 SKSEPlugin_Load 中调用（早于 kDataLoaded，但 QLIE 弹菜单发生在游戏中，
    // 那时 DLL 必已加载）。返回是否探测成功。
    [[nodiscard]] bool install();
}
