#pragma once

namespace UiMenu
{
    // 游戏数据加载完成（kDataLoaded）后调用：此时 SKSEMenuFramework.dll 已加载。
    // 探测框架可用性并注册 Highlight Lootable Corpses 参数面板到 Mod Control Panel。
    void register_menus();

    // Mod Control Panel 的任一窗口（含本插件设置页）当前是否打开。
    // 未安装 SKSE Menu Framework 时恒为 false。供输入侧在面板打开期间
    // 暂停热键响应/取消热键捕获使用。
    [[nodiscard]] bool is_menu_open();
}
