//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

#include "mask_types.h"

MASK_NAMESPACE_BEGIN

// Palette length P = min(skinData bone count, skin world matrix count). A vertex bone index is a
// **global subscript** into the skin bone array, so the palette is built in global index space and
// P is its valid length. This is the single authoritative implementation, shared by the skinned
// collection (validation bound/guard) and the mask draw (palette build/guard).
[[nodiscard]] uint32_t palette_slot_count(RE::NiSkinInstance const* a_skin);

// Diagnostic logging for the skinned path's failure exits - emitted once per (node name, reason)
// signature (with values attached); the same signature is not repeated. Shared inside the mask
// subsystem (collection and draw-time guards).
void log_skinned_skip_once(bool a_warn, char const* a_node_name, std::string_view a_reason, std::string_view a_details);

// Collect this frame's geometry draws from the mask targets along two paths, static (the
// BSTriShape family) and skinned (NiSkinPartition partitions), including position-format and
// skin-layout self-calibration and per-mesh validation - draws with no solution are skipped
// (better to draw too little than to smear garbage over the screen). The order of the target list
// is the corpse index, and a target beyond the slots is clamped to the last index.
void collect_mask_draws(std::vector<MaskTarget> const& a_targets, std::vector<MaskDraw>& a_draws);

MASK_NAMESPACE_END
