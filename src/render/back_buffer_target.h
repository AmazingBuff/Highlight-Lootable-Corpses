//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11RenderTargetView;
struct ID3D11Texture2D;
struct IDXGISwapChain;

PLUGIN_NAMESPACE_BEGIN

// 后台缓冲 RTV 缓存：交换链缓冲对象变化或首帧时重建，尺寸/格式取自交换链纹理描述。
// 对象为进程级生存期（与原实现一致：不在析构中释放，避免 DLL 卸载期与设备销毁竞争）。
class BackBufferTarget
{
public:
    // 确保后台缓冲 RTV 有效；失败返回 false（调用方跳过本帧）。
    bool ensure(IDXGISwapChain* a_swap_chain, ID3D11Device* a_device);

    [[nodiscard]] ID3D11RenderTargetView* rtv() const { return m_render_target; }
    [[nodiscard]] std::uint32_t width() const { return m_width; }
    [[nodiscard]] std::uint32_t height() const { return m_height; }

private:
    ID3D11RenderTargetView* m_render_target = nullptr;
    ID3D11Texture2D* m_back_buffer = nullptr;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
};

PLUGIN_NAMESPACE_END
