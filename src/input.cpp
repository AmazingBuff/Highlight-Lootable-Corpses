#include "pch.h"
#include "input.h"
#include "config.h"

namespace Input
{
    namespace
    {
        bool g_was_down = false;

        // 热键重绑定状态：begin_rebind 在 MCP 菜单线程（游戏主线程）调用，
        // poll/take_rebind_result 在渲染回调线程调用 → 状态经原子量传递
        std::atomic<bool> g_rebind{ false };
        std::atomic<bool> g_rebind_done{ false };
        std::atomic<std::uint32_t> g_rebind_vk{ 0 };
        bool g_rebind_prev_down[0xFF] = {};  // 捕获用的逐键上轮按下状态（仅 poll 线程触碰）

        void poll_rebind()
        {
            // 跳过鼠标键（0x01-0x07）：热键限定键盘；捕获任意按键（含 ESC = 取消）
            for (std::uint32_t vk = 0x08; vk <= 0xFE; ++vk)
            {
                bool const down = (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
                bool& prev = g_rebind_prev_down[vk];
                if (down && !prev)
                {
                    g_rebind_vk.store(vk == VK_ESCAPE ? 0u : vk, std::memory_order_relaxed);
                    g_rebind_done.store(true, std::memory_order_relaxed);
                    g_rebind.store(false, std::memory_order_relaxed);
                    prev = down;
                    return;
                }
                prev = down;
            }
        }
    }

    void poll()
    {
        if (g_rebind.load(std::memory_order_relaxed))
        {
            poll_rebind();
            return;  // 捕获期间暂停热键 toggle，避免捕获的键同时被当作热键触发
        }

        uint32_t const vk = Config::get().hotkey;
        if (vk == 0)
            return;

        bool const down = (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
        if (down && !g_was_down)
        {
            bool const enabled = !Config::is_enabled();
            Config::set_enabled(enabled);
            // 控制台消息与 INI 写回放到游戏线程执行
            SKSE::GetTaskInterface()->AddTask([enabled]()
            {
                // 主菜单/控制台未创建时单例为空
                if (RE::ConsoleLog* console = RE::ConsoleLog::GetSingleton())
                    console->Print("HighlightLootableCorpses: %s", enabled ? "ON" : "OFF");

                Config::save_enabled();
            });
        }
        g_was_down = down;
    }

    void begin_rebind()
    {
        g_rebind_vk.store(0, std::memory_order_relaxed);
        g_rebind_done.store(false, std::memory_order_relaxed);
        g_rebind.store(true, std::memory_order_relaxed);
    }

    bool take_rebind_result(std::uint32_t& a_out)
    {
        if (!g_rebind_done.exchange(false, std::memory_order_relaxed))
            return false;
        a_out = g_rebind_vk.exchange(0, std::memory_order_relaxed);
        return true;
    }
}
