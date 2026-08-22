#include "pch.h"
#include "input.h"
#include "config.h"

namespace Input
{
    namespace
    {
        bool g_was_down = false;
    }

    void poll()
    {
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
                    console->Print("CorpseESP: %s", enabled ? "ON" : "OFF");

                Config::save_enabled();
            });
        }
        g_was_down = down;
    }
}
