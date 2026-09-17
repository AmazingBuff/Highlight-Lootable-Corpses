//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

#include "Plugin.h"

#include <RE/Skyrim.h>

#include <DirectXMath.h>
#include <d3d11.h>

#include <cstddef>
#include <cstdint>

#define MASK_NAMESPACE_BEGIN PLUGIN_NAMESPACE_BEGIN namespace Mask {
#define MASK_NAMESPACE_END PLUGIN_NAMESPACE_END }

MASK_NAMESPACE_BEGIN

// ---------------------------------------------------------------------------
// 蒙皮调色板常量缓冲预算（矩阵/draw）；128×64B = 8KB。
// ---------------------------------------------------------------------------
inline constexpr size_t Max_Palette_Bones = 128;
inline constexpr size_t Palette_CB_Bytes = Max_Palette_Bones * 64;
static_assert(Palette_CB_Bytes == Max_Palette_Bones * sizeof(float) * 16, "palette CB layout must be float4x4 slots");

// mask RT 清屏色
inline constexpr float Mask_Clear_Color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };


// 蒙皮顶点缓冲内的权重/索引布局（自标定结果）
struct MaskSkinLayout
{
    DXGI_FORMAT weight_format;
    uint32_t weight_offset;
    DXGI_FORMAT index_format;
    uint32_t index_offset;
};

// mask 渲染目标（引用 + 距离衰减不透明度，按目标序号即尸体索引）
struct MaskTarget
{
    RE::NiPointer<RE::TESObjectREFR> ref;
    DirectX::XMFLOAT4 color;
};

// 一条几何 draw（渲染线程独占的临时列表；VB/IB 为游戏对象借用）。生存期说明：
// 静态路径借用的 node/rendererData/VB/IB 由 node_ref（几何体 NiPointer）保活，
// 防止游戏线程在本帧收集与绘制之间卸载目标 3D 导致悬垂；蒙皮路径的
// buffData/VB/IB 由 skin（NiSkinInstance→skinPartition→buffData）引用链保活。
struct MaskDraw
{
    ID3D11Buffer* vertex_buffer;
    ID3D11Buffer* index_buffer;
    RE::BSGraphics::VertexDesc vertex_desc;
    RE::NiAVObject* node;  // 静态路径的世界变换来源（借用，生存期见 node_ref）
    RE::NiPointer<RE::BSGeometry> node_ref;  // 保活静态路径的几何体（及其 GPU 缓冲）
    uint32_t vertex_stride;
    uint32_t vertex_count;
    uint32_t triangle_count;
    uint32_t index_count;
    // 位置属性布局：静态路径写入 calibrate_position_format 结果（UNKNOWN 表示按
    // desc 推导）；蒙皮路径写入属性偏移间距判定结果（绝不为 UNKNOWN）。
    DXGI_FORMAT position_format;
    uint32_t position_offset;
    bool skinned;                            // true：按分区 draw，调色板蒙皮
    RE::NiPointer<RE::NiSkinInstance> skin;  // 保活蒙皮实例（骨骼世界矩阵）
    uint32_t partition;
    uint32_t target_index;
    // 蒙皮权重/索引布局：仅蒙皮 draw 写入标定结果；静态 draw 保持默认值。
    MaskSkinLayout skin_layout;
};

MASK_NAMESPACE_END
