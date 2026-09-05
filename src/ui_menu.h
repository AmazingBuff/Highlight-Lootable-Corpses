#pragma once

namespace UiMenu
{
    // 游戏数据加载完成（kDataLoaded）后调用：此时 SKSEMenuFramework.dll 已加载。
    // 探测框架可用性并注册 Highlight Lootable Corpses 参数面板到 Mod Control Panel。
    void register_menus();
}
