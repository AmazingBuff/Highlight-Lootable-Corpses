//
// Created by AmazingBuff on 2026/9/13.
//

#include "present_hook.h"

#include "Plugin.h"

#include <RE/Skyrim.h>

PLUGIN_NAMESPACE_BEGIN

PresentHook& PresentHook::instance()
{
    static PresentHook s_instance;
    return s_instance;
}

HRESULT STDMETHODCALLTYPE PresentHook::present_thunk(IDXGISwapChain* a_swapChain, UINT a_syncInterval, UINT a_flags)
{
    instance().m_callback(a_swapChain);
    return instance().m_ref_original_present(a_swapChain, a_syncInterval, a_flags);
}

bool PresentHook::install(Callback a_on_present)
{
    if (m_installed)
        return true;  // already installed
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
    // IDXGISwapChain::Present sits in vtable slot 8 (IUnknown×3 + IDXGIObject×4 + GetDevice); the
    // vtable address is taken from the runtime object itself, so it does not depend on an Address
    // Library ID and is valid for any AE version. write_vfunc handles the page protection
    // automatically through safe_write and returns the original function pointer.
    REL::Relocation<uintptr_t> vtable{ reinterpret_cast<uintptr_t>(*reinterpret_cast<void**>(swap_chain)) };
    m_ref_original_present = reinterpret_cast<PresentFunc>(vtable.write_vfunc(8, &PresentHook::present_thunk));
    m_callback = a_on_present;
    m_installed = true;

    logger::info("Installed IDXGISwapChain::Present hook (swap chain={}, original={})", fmt::ptr(swap_chain), fmt::ptr(m_ref_original_present));
    return true;
}

PresentHook::PresentHook() : m_ref_original_present(nullptr), m_callback(nullptr), m_installed(false) {}

PresentHook::~PresentHook() = default;

PLUGIN_NAMESPACE_END
