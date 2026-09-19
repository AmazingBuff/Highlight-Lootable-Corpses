//
// Created by AmazingBuff on 2026/9/13.
//

#include "present_hook.h"

PLUGIN_NAMESPACE_BEGIN

PresentHook& PresentHook::instance()
{
    static PresentHook s_instance;
    return s_instance;
}

REX::W32::HRESULT PresentHook::present_thunk(REX::W32::IDXGISwapChain* swap_chain, uint32_t sync_interval, uint32_t flags)
{
    instance().m_callback(swap_chain);
    return instance().m_ref_original_present(swap_chain, sync_interval, flags);
}

bool PresentHook::install(Callback on_present)
{
    if (!on_present)
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

    REX::W32::IDXGISwapChain* swap_chain = rt.renderWindows[0].swapChain;
    // The swap chain's Present sits in vtable slot 8 (IUnknown×3 + four base-interface methods + GetDevice); the
    // vtable address is taken from the runtime object itself, so it does not depend on an Address
    // Library ID and is valid for any AE version. write_vfunc handles the page protection
    // automatically through safe_write and returns the original function pointer.
    REL::Relocation<uintptr_t> vtable{ reinterpret_cast<uintptr_t>(*reinterpret_cast<void**>(swap_chain)) };
    m_ref_original_present = reinterpret_cast<PresentFunc>(vtable.write_vfunc(8, &PresentHook::present_thunk));
    m_callback = on_present;

    logger::info("Hook swap chain present");
    return true;
}

PresentHook::PresentHook() : m_ref_original_present(nullptr), m_callback(nullptr) {}

PresentHook::~PresentHook() = default;

PLUGIN_NAMESPACE_END
