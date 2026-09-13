//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

#include <d3d11.h>

#include <cstddef>
#include <cstdint>

#include <RE/B/BSGeometry.h>
#include <RE/N/NiSmartPointer.h>
#include <RE/N/NiTransform.h>

PLUGIN_NAMESPACE_BEGIN

// ---------------------------------------------------------------------------
// 蒙皮调色板常量缓冲预算（矩阵/draw）；128×64B = 8KB。
// ---------------------------------------------------------------------------
inline constexpr std::size_t Max_Palette_Bones = 128;
inline constexpr std::size_t Palette_CB_Bytes = Max_Palette_Bones * 64;
static_assert(Palette_CB_Bytes == Max_Palette_Bones * sizeof(float) * 16, "palette CB layout must be float4x4 slots");

// ---------------------------------------------------------------------------
// alpha LUT（按目标序号存放 corpse_alpha，消费 PS 以 mask B 通道的尸体索引查表）；
// 256 个 alpha 以 64 个 float4 打包（1024 字节，CB 尺寸 16 对齐）。
// ---------------------------------------------------------------------------
inline constexpr std::size_t Alpha_Lut_Floats = 256;
inline constexpr std::size_t Alpha_Lut_CB_Bytes = Alpha_Lut_Floats * sizeof(float);
static_assert(Alpha_Lut_CB_Bytes == 64 * sizeof(float) * 4);

// ---------------------------------------------------------------------------
// 矩阵工具：行主序 4x4，数学约定（列向量），clip = M · v。
// 引擎矩阵消费约定由实验实证：
//  - NiTransform 原样消费（entry[row][col] 直接作为列向量矩阵元素）算得的
//    世界包围球心与引擎 worldBound.center 完全一致，转置消费偏差明显——
//    因此 from_transform 不转置；
//  - worldToCam 原样消费（from_world_to_cam_raw，不转置）——五点解算实证
//    （1e-7 一致性）：成员 WorldPtToScreenPt3 等价于 clip = W2C_raw·p 的列向量
//    消费 + /w 归一化，视锥斜率已烘焙在行内（行 0/1 的非单位范数即水平/垂直
//    NDC 缩放），无需视锥矩阵与端口映射。
// 刻意不用 BSGraphics::State 的 viewProj——AE 实测有坏值（见 screen_projector.cpp）。
// ---------------------------------------------------------------------------
struct MaskMat4
{
    float m[4][4];

    static constexpr MaskMat4 identity()
    {
        return MaskMat4{ { { 1.0f, 0.0f, 0.0f, 0.0f },
                           { 0.0f, 1.0f, 0.0f, 0.0f },
                           { 0.0f, 0.0f, 1.0f, 0.0f },
                           { 0.0f, 0.0f, 0.0f, 1.0f } } };
    }

    // NiTransform → 4x4：原样消费 entry[row][col]（不转置）。
    static MaskMat4 from_transform(RE::NiTransform const& a_transform)
    {
        MaskMat4 r = identity();
        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 3; ++col)
                r.m[row][col] = a_transform.rotate.entry[row][col] * a_transform.scale;
            r.m[row][3] = a_transform.translate[row];
        }
        return r;
    }

    // 引擎 worldToCam → MaskMat4：原样行主序拷贝（不转置）。
    static MaskMat4 from_world_to_cam_raw(float const (&a_src)[4][4])
    {
        MaskMat4 r{};
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                r.m[row][col] = a_src[row][col];
        return r;
    }

    MaskMat4 transposed() const
    {
        MaskMat4 r{};
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                r.m[row][col] = m[col][row];
        return r;
    }

    MaskMat4 operator*(MaskMat4 const& a_rhs) const  // (A*B)(v) = A(B(v))
    {
        MaskMat4 r{};
        for (int row = 0; row < 4; ++row)
        {
            for (int col = 0; col < 4; ++col)
            {
                r.m[row][col] = m[row][0] * a_rhs.m[0][col] +
                                m[row][1] * a_rhs.m[1][col] +
                                m[row][2] * a_rhs.m[2][col] +
                                m[row][3] * a_rhs.m[3][col];
            }
        }
        return r;
    }
};

// 蒙皮顶点缓冲内的权重/索引布局（自标定结果）
struct MaskSkinLayout
{
    DXGI_FORMAT weight_format = DXGI_FORMAT_UNKNOWN;
    std::uint32_t weight_offset = 0;
    DXGI_FORMAT index_format = DXGI_FORMAT_UNKNOWN;
    std::uint32_t index_offset = 0;
};

// mask 渲染目标（引用 + 距离衰减不透明度，按目标序号即尸体索引）
struct MaskTarget
{
    RE::NiPointer<RE::TESObjectREFR> ref;
    float opacity;
};

// 一条几何 draw（渲染线程独占的临时列表；VB/IB 为游戏对象借用）。生存期说明：
// 静态路径借用的 node/rendererData/VB/IB 由 node_ref（几何体 NiPointer）保活，
// 防止游戏线程在本帧收集与绘制之间卸载目标 3D 导致悬垂；蒙皮路径的
// buffData/VB/IB 由 skin（NiSkinInstance→skinPartition→buffData）引用链保活。
struct MaskDraw
{
    ID3D11Buffer* vertex_buffer = nullptr;
    ID3D11Buffer* index_buffer = nullptr;
    RE::BSGraphics::VertexDesc vertex_desc;
    RE::NiAVObject* node = nullptr;  // 静态路径的世界变换来源（借用，生存期见 node_ref）
    RE::NiPointer<RE::BSGeometry> node_ref;  // 保活静态路径的几何体（及其 GPU 缓冲）
    std::uint32_t vertex_stride = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t triangle_count = 0;
    std::uint32_t index_count = 0;
    // 位置属性布局：静态路径写入 calibrate_position_format 结果（UNKNOWN 表示按
    // desc 推导）；蒙皮路径写入属性偏移间距判定结果（绝不为 UNKNOWN）。
    DXGI_FORMAT position_format = DXGI_FORMAT_UNKNOWN;
    std::uint32_t position_offset = 0;
    bool skinned = false;                    // true：按分区 draw，调色板蒙皮
    RE::NiPointer<RE::NiSkinInstance> skin;  // 保活蒙皮实例（骨骼世界矩阵）
    std::uint32_t partition = 0;
    // 尸体索引（目标序号/255），经 per-draw CB → VS → mask PS 的 B 通道输出
    //（MAX 混合取重叠尸体的较高索引），消费 pass 据此查 alpha LUT。
    float corpse_index = 0.0f;
    // 蒙皮权重/索引布局：仅蒙皮 draw 写入标定结果；静态 draw 保持默认值。
    MaskSkinLayout skin_layout{};
};

PLUGIN_NAMESPACE_END
