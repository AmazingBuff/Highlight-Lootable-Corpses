//
// Created by AmazingBuff on 2026/9/13.
//

#include "present_hook.h"

#include <RE/Skyrim.h>

PLUGIN_NAMESPACE_BEGIN

namespace
{
    using PresentFunc = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

    PresentFunc g_original_present = nullptr;
    PresentHook::Callback g_callback = nullptr;
    bool g_installed = false;

    HRESULT STDMETHODCALLTYPE present_thunk(IDXGISwapChain* a_swapChain, UINT a_syncInterval, UINT a_flags)
    {
        g_callback(a_swapChain);
        return g_original_present(a_swapChain, a_syncInterval, a_flags);
    }
}

bool PresentHook::install(Callback a_on_present)
{
    if (g_installed)
        return true;  // 已安装
    if (!a_on_present)
        return false;

    RE::BSGraphics::Renderer* renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!renderer)
        return false;

    RE::BSGraphics::RendererData& rt = renderer->GetRuntimeData();
    if (!rt.renderWindows[0].swapChain)
    {
        logger::warn("SwapChain not available yet, will retry on next game message");
        return false;
    }

    IDXGISwapChain* swap_chain = reinterpret_cast<IDXGISwapChain*>(rt.renderWindows[0].swapChain);
    // IDXGISwapChain::Present 位于 vtable 第 8 槽位（IUnknown×3 + IDXGIObject×4 + GetDevice）；
    // vtable 地址取自运行时对象本身，不依赖 Address Library ID，任何 AE 版本都有效。
    // write_vfunc 内部经 safe_write 自动处理页保护，并返回原始函数指针。
    REL::Relocation<std::uintptr_t> vtable{ reinterpret_cast<std::uintptr_t>(*reinterpret_cast<void**>(swap_chain)) };
    g_original_present = reinterpret_cast<PresentFunc>(vtable.write_vfunc(8, &present_thunk));
    g_callback = a_on_present;
    g_installed = true;

    logger::info("Installed IDXGISwapChain::Present hook (swap chain={}, original={})", fmt::ptr(swap_chain), fmt::ptr(g_original_present));
    return true;
}

PLUGIN_NAMESPACE_END
