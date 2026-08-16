#include "PCH.h"
#include "Input.h"
#include "Config.h"

namespace
{
    bool g_wasDown = false;
}

namespace Input
{
    void Poll()
    {
        auto const vk = Config::Get().hotkey;
        if (vk == 0)
        {
            return;
        }

        bool const down = (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
        if (down && !g_wasDown)
        {
            bool const enabled = !Config::IsEnabled();
            Config::SetEnabled(enabled);
            // 控制台消息与 INI 写回放到游戏线程执行
            SKSE::GetTaskInterface()->AddTask([enabled]() {
                RE::ConsoleLog::GetSingleton()->Print("CorpseESP: %s", enabled ? "ON" : "OFF");
                Config::SaveEnabled();
            });
        }
        g_wasDown = down;
    }
}
