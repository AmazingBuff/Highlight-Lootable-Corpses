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
		const auto vk = Config::Get().hotkey;
		if (vk == 0) {
			return;
		}

		const bool down = (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
		if (down && !g_wasDown) {
			const bool enabled = !Config::IsEnabled();
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
