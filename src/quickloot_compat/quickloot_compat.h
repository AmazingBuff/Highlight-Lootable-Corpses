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
    // 必须在 kPostLoad 消息（所有插件的 SKSEPlugin_Load 均已返回）之后调用：
    // SKSE 逐个加载插件（NTFS 枚举序 ≈ 字母序，H 先于 Q），本插件 Load 时
    // QuickLootIE.dll 尚未进入进程，过早探测必然失败。内部幂等，可重复调用。
    // 返回是否探测并注册成功。
    [[nodiscard]] bool install();
}
