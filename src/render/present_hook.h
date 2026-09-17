//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

#include "Plugin.h"

#include <d3d11.h>

PLUGIN_NAMESPACE_BEGIN

// IDXGISwapChain::Present vtable 钩子（vtable 第 8 槽位，经 REL::Relocation::
// write_vfunc 写入，页保护由其内部 safe_write 处理并返回原始函数指针）。
// 运行时无关：不依赖 Address Library ID，任何 AE 版本都有效。
// 回调先于原 Present 执行；游戏线程 Present hook 可能被多个线程并发进入，
// 回调内部自行串行化。
class PresentHook
{
public:
    using Callback = void (STDMETHODCALLTYPE*)(IDXGISwapChain* a_swap_chain);

    static PresentHook& instance();

    // 幂等安装；交换链尚未就绪时返回 false 并 WARN（调用方在后续游戏消息中重试）。
    bool install(Callback a_on_present);
private:
    PresentHook();
    ~PresentHook();

    using PresentFunc = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

    static HRESULT STDMETHODCALLTYPE present_thunk(IDXGISwapChain* a_swap_chain, UINT a_sync_interval, UINT a_flags);
private:
    PresentFunc m_ref_original_present;
    Callback m_callback;
    bool m_installed;
};

PLUGIN_NAMESPACE_END
