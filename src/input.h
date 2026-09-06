#pragma once

#include <cstdint>

namespace Input
{
    // 每帧从渲染回调调用：轮询热键并处理开关切换；热键重绑定的按键捕获也在此处理
    void poll();

    // MCP 菜单调用：进入"捕获下一次按键"状态（作为新的热键）。全量捕获所有
    // 按键（键盘 + 鼠标）：ESC/F1 这类会触发 Menu Framework 面板开合的键同样
    // 可以绑定（面板可能被关闭，但捕获继续有效）。捕获期间热键 toggle 暂停；
    // 捕获态有 5 秒超时，再次点击 rebind 按钮也可取消（cancel_rebind）。poll
    // 线程（渲染回调）与 MCP 菜单线程不同，状态经原子量传递。
    void begin_rebind();
    void cancel_rebind();

    // MCP 菜单每帧轮询：rebind 结束时返回 true 并退出捕获态。
    // a_out = 捕获到的 VK 码；0 表示取消（点击取消 / MCP 窗口被关闭）。
    [[nodiscard]] bool take_rebind_result(std::uint32_t& a_out);
}
