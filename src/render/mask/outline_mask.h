//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;

namespace RE
{
    class NiCamera;
    class TESObjectREFR;
}

PLUGIN_NAMESPACE_BEGIN

// RGB is unpremultiplied; opacity already includes configuration alpha, pulse and fade.
struct OutlineMaskTarget
{
    RE::TESObjectREFR* ref;
    DirectX::XMFLOAT4 color;
};

// Render-thread-only facade. Targets are retained by NiPointer. Silhouettes use
// private depth; outlines merge independent whole-target masks. Both ignore scene depth.
class OutlineMask
{
public:
    OutlineMask() = delete;
    ~OutlineMask() = delete;
    OutlineMask(OutlineMask const&) = delete;
    OutlineMask(OutlineMask const&&) = delete;
    OutlineMask operator=(OutlineMask&) = delete;
    OutlineMask operator=(OutlineMask&&) = delete;

    static void set_targets(std::vector<OutlineMaskTarget> const& a_targets);

    // 由 Present 回调调用：收集 a_camera 视角下的几何并渲染 mask，随后按当前
    // display_mode 把填充/描边叠加到 a_overlay_target 上——调用方须传 Present
    // 将展示的后备缓冲的 RTV（契约与 icon 路径一致，不依赖引擎 Present 时刻
    // 碰巧绑定的渲染目标）。
    // a_width/a_height 为后备缓冲尺寸（mask RT 与其同尺寸，变化时重建）。
    static void render(ID3D11Device* a_device, ID3D11DeviceContext* a_context, RE::NiCamera* a_camera,
        ID3D11RenderTargetView* a_overlay_target, std::uint32_t a_width, std::uint32_t a_height);
};

PLUGIN_NAMESPACE_END
