//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

PLUGIN_NAMESPACE_BEGIN

// REX::W32::IDXGISwapChain::Present vtable hook (vtable slot 8, written through REL::Relocation::write_vfunc,
// whose internal safe_write handles the page protection and returns the original function pointer).
// Runtime-agnostic: it does not depend on an Address Library ID and is valid for any AE version.
// The callback runs before the original Present; the game-thread Present hook can be entered
// concurrently by several threads, so the callback serialises itself internally.
class PresentHook
{
public:
    using Callback = void (*)(REX::W32::IDXGISwapChain* swap_chain);

    static PresentHook& instance();

    // Idempotent install; returns false with a WARN while the swap chain is not ready yet (the caller retries on a later game message).
    bool install(Callback on_present);
private:
    PresentHook();
    ~PresentHook();

    using PresentFunc = REX::W32::HRESULT(*)(REX::W32::IDXGISwapChain*, uint32_t, uint32_t);

    static REX::W32::HRESULT present_thunk(REX::W32::IDXGISwapChain* swap_chain, uint32_t sync_interval, uint32_t flags);
private:
    PresentFunc m_ref_original_present;
    Callback m_callback;
};

PLUGIN_NAMESPACE_END
