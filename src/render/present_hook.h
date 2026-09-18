//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

#include "Plugin.h"

#include <combaseapi.h>

struct IDXGISwapChain;

PLUGIN_NAMESPACE_BEGIN

// IDXGISwapChain::Present vtable hook (vtable slot 8, written through REL::Relocation::write_vfunc,
// whose internal safe_write handles the page protection and returns the original function pointer).
// Runtime-agnostic: it does not depend on an Address Library ID and is valid for any AE version.
// The callback runs before the original Present; the game-thread Present hook can be entered
// concurrently by several threads, so the callback serialises itself internally.
class PresentHook
{
public:
    using Callback = void (STDMETHODCALLTYPE*)(IDXGISwapChain* a_swap_chain);

    static PresentHook& instance();

    // Idempotent install; returns false with a WARN while the swap chain is not ready yet (the caller retries on a later game message).
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
