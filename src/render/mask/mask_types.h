//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

#define MASK_NAMESPACE_BEGIN PLUGIN_NAMESPACE_BEGIN namespace Mask {
#define MASK_NAMESPACE_END PLUGIN_NAMESPACE_END }

MASK_NAMESPACE_BEGIN

// ---------------------------------------------------------------------------
// Skinned palette constant-buffer budget (matrices per draw); 128×64B = 8KB.
// ---------------------------------------------------------------------------
inline constexpr size_t Max_Palette_Bones = 128;
inline constexpr size_t Palette_CB_Bytes = Max_Palette_Bones * 64;
static_assert(Palette_CB_Bytes == Max_Palette_Bones * sizeof(float) * 16, "palette CB layout must be float4x4 slots");

// mask RT clear color
inline constexpr float Mask_Clear_Color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };


// Weight/index layout inside the skinned vertex buffer (self-calibration result)
struct MaskSkinLayout
{
    DXGI_FORMAT weight_format;
    uint32_t weight_offset;
    DXGI_FORMAT index_format;
    uint32_t index_offset;
};

// mask render target (a reference plus a distance-faded opacity, ordered by target number, i.e. the corpse index)
struct MaskTarget
{
    RE::NiPointer<RE::TESObjectREFR> ref;
    DirectX::XMFLOAT4 color;
};

// One geometry draw (a temporary list owned by the render thread; VB/IB are borrowed from game
// objects). Lifetime: the node/rendererData/VB/IB borrowed by the static path are kept alive by
// node_ref (a geometry NiPointer), so the game thread cannot unload the target 3D between
// collection and drawing within the same frame and leave them dangling; the skinned path keeps
// its buffData/VB/IB alive through the skin (NiSkinInstance→skinPartition→buffData) reference chain.
struct MaskDraw
{
    ID3D11Buffer* vertex_buffer;
    ID3D11Buffer* index_buffer;
    RE::BSGraphics::VertexDesc vertex_desc;
    RE::NiAVObject* node;  // world transform source of the static path (borrowed; lifetime described at node_ref)
    RE::NiPointer<RE::BSGeometry> node_ref;  // keeps the static-path geometry (and its GPU buffers) alive
    uint32_t vertex_stride;
    uint32_t vertex_count;
    uint32_t triangle_count;
    uint32_t index_count;
    // Position attribute layout: the static path stores the calibrate_position_format result
    // (UNKNOWN means derive from desc); the skinned path stores the attribute-offset spacing result
    // (never UNKNOWN).
    DXGI_FORMAT position_format;
    uint32_t position_offset;
    bool skinned;                            // true: draw per partition with palette skinning
    RE::NiPointer<RE::NiSkinInstance> skin;  // keeps the skin instance alive (bone world matrices)
    uint32_t partition;
    uint32_t target_index;
    // Skinning weight/index layout: only skinned draws store a calibration result; static draws keep the default values.
    MaskSkinLayout skin_layout;
};

MASK_NAMESPACE_END
