//
// Created by AmazingBuff on 2026/08/20.
//

#pragma once

#include <cstdint>
#include <memory>

#include <d3d11.h>

namespace RE
{
    class TESObjectREFR;
}

namespace MeshOutline
{
    // 一具尸体的可绘制网格部件集合：扫描期（游戏线程）采集，渲染线程只读共享。
    // 内部对引擎对象一律持 NiPointer，保证渲染期引用的几何/蒙皮数据不会被释放。
    struct DrawList;
    using DrawListPtr = std::shared_ptr<DrawList const>;

    // 游戏线程：遍历 ref 的 3D 树采集可绘制部件；无可用网格时返回空指针
    [[nodiscard]] DrawListPtr collect(RE::TESObjectREFR* a_ref);

    [[nodiscard]] std::size_t part_count(DrawListPtr const& a_list) noexcept;

    // 渲染线程：懒创建资源、清空遮罩并切到遮罩绘制状态。
    // 返回 false 表示剪影不可用（调用方应退回包围盒描边）。
    [[nodiscard]] bool begin_frame(ID3D11Device* a_device, ID3D11DeviceContext* a_context, std::uint32_t a_width, std::uint32_t a_height);

    // 渲染线程：把一具尸体的网格以 a_alpha 写入遮罩（不绑深度，穿墙可见）
    void draw(ID3D11DeviceContext* a_context, DrawListPtr const& a_list, float const a_world_to_cam[4][4], float a_alpha);

    // 渲染线程：遮罩 -> 描边并混合到 a_target，随后恢复 begin_frame 时的 viewport。
    // 必须与返回 true 的 begin_frame 成对调用（即使一次 draw 都没有发生）。
    void resolve(ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_target, float a_thickness, float a_red, float a_green, float a_blue);

    void release() noexcept;
}
