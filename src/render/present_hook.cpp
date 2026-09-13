//
// Created by AmazingBuff on 2026/9/13.
//

#include "present_hook.h"

#include <Windows.h>

#include <RE/Skyrim.h>

PLUGIN_NAMESPACE_BEGIN

namespace
{
    using PresentFunc = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

    PresentFunc g_original_present = nullptr;
    void** g_hooked_slot = nullptr;
    PresentHook::Callback g_callback = nullptr;

    HRESULT STDMETHODCALLTYPE present_thunk(IDXGISwapChain* a_swapChain, UINT a_syncInterval, UINT a_flags)
    {
        g_callback(a_swapChain);
        return g_original_present(a_swapChain, a_syncInterval, a_flags);
    }
}

bool PresentHook::install(Callback a_on_present)
{
    if (g_hooked_slot)
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
    void** vtable = *reinterpret_cast<void***>(swap_chain);

    // IDXGISwapChain::Present 是虚函数表中第 8 个槽位
    g_hooked_slot = &vtable[8];
    g_original_present = reinterpret_cast<PresentFunc>(*g_hooked_slot);
    g_callback = a_on_present;

    DWORD old_protect = 0;
    if (!VirtualProtect(g_hooked_slot, sizeof(void*), PAGE_READWRITE, &old_protect))
    {
        logger::error("VirtualProtect failed, cannot install Present hook");
        g_hooked_slot = nullptr;
        g_original_present = nullptr;
        g_callback = nullptr;
        return false;
    }
    *g_hooked_slot = reinterpret_cast<void*>(&present_thunk);
    VirtualProtect(g_hooked_slot, sizeof(void*), old_protect, &old_protect);

    logger::info("Installed IDXGISwapChain::Present hook (swap chain={}, original={})", fmt::ptr(swap_chain), fmt::ptr(g_original_present));
    return true;
}

PLUGIN_NAMESPACE_END
