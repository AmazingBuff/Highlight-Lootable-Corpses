#include "pch.h"
#include "input.h"
#include "config.h"
#include "ui_menu.h"

namespace Input
{
    namespace
    {
        bool g_was_down = false;

        constexpr std::int64_t kRebindTimeoutMs = 5000;  // 捕获态超时：防捕获态悬挂

        // 热键重绑定状态：begin_rebind 在 MCP 菜单线程（游戏主线程）调用，
        // poll/take_rebind_result 在渲染回调线程调用 → 状态经原子量传递
        std::atomic<bool> g_rebind{ false };
        std::atomic<bool> g_rebind_done{ false };
        std::atomic<std::uint32_t> g_rebind_vk{ 0 };
        std::atomic<std::int64_t> g_rebind_start_ms{ 0 };
        bool g_rebind_prev_down[0xFF] = {};  // 捕获用的逐键上轮按下状态（仅 poll 线程触碰）

        std::int64_t now_ms()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        void poll_rebind()
        {
            // 全量捕获所有键（键盘 + 鼠标）：包括 ESC/F1 这类会触发 Menu Framework
            // 面板开合的键与鼠标键——GetAsyncKeyState 是全局轮询，面板被这些键关闭
            // 后捕获继续有效（5 秒超时兜底）。仅跳过 0x03（Ctrl+Break 组合）与
            // 0x07（未定义）。注意：绑面板开合键/攻击键意味着与游戏交互共用按键，
            // 属用户自己的取舍。
            for (std::uint32_t vk = 0x01; vk <= 0xFE; ++vk)
            {
                if (vk == 0x03 || vk == 0x07)
                    continue;

                bool const down = (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
                bool& prev = g_rebind_prev_down[vk];
                if (down && !prev)
                {
                    g_rebind_vk.store(vk, std::memory_order_relaxed);
                    g_rebind_done.store(true, std::memory_order_relaxed);
                    g_rebind.store(false, std::memory_order_relaxed);
                    prev = down;
                    // 绑定完成时按键仍处于按下状态：标记为已按下，避免同一 down
                    // 跳变在下一帧立即触发一次热键切换
                    g_was_down = true;
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
            // 超时兜底：捕获态下迟迟没有按键（面板被 ESC/F1 关闭后用户没有继续
            // 操作等）自动取消，避免捕获态无限悬挂、把普通游戏按键绑成热键。
            // 捕获不随面板关闭立即取消——否则面板开合键（ESC/F1）永远无法绑定。
            if (now_ms() - g_rebind_start_ms.load(std::memory_order_relaxed) > kRebindTimeoutMs)
            {
                cancel_rebind();
                return;
            }
            poll_rebind();
            return;  // 捕获期间暂停热键 toggle，避免捕获的键同时被当作热键触发
        }

        // MCP 面板打开期间不响应热键，避免在设置界面内误触发开关
        if (UiMenu::is_menu_open())
            return;

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
        g_rebind_start_ms.store(now_ms(), std::memory_order_relaxed);
        g_rebind.store(true, std::memory_order_relaxed);
    }

    void cancel_rebind()
    {
        g_rebind_vk.store(0, std::memory_order_relaxed);
        g_rebind_done.store(true, std::memory_order_relaxed);
        g_rebind.store(false, std::memory_order_relaxed);
    }

    bool take_rebind_result(std::uint32_t& a_out)
    {
        if (!g_rebind_done.exchange(false, std::memory_order_relaxed))
            return false;
        a_out = g_rebind_vk.exchange(0, std::memory_order_relaxed);
        return true;
    }
}
