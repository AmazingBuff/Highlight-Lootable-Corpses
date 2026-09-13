//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

#include <d3d11.h>

PLUGIN_NAMESPACE_BEGIN

// IDXGISwapChain::Present vtable 钩子（vtable 第 8 槽位，VirtualProtect 改写）。
// 运行时无关：不依赖 Address Library ID，任何 AE 版本都有效。
// 回调先于原 Present 执行；游戏线程 Present hook 可能被多个线程并发进入，
// 回调内部自行串行化。
class PresentHook
{
public:
    using Callback = void (STDMETHODCALLTYPE*)(IDXGISwapChain* a_swap_chain);

    // 幂等安装；交换链尚未就绪时返回 false 并 WARN（调用方在后续游戏消息中重试）。
    static bool install(Callback a_on_present);
};

PLUGIN_NAMESPACE_END
