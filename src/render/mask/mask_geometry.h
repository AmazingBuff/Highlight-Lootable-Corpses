//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "mask_types.h"

namespace RE
{
	class NiSkinInstance;
}

MASK_NAMESPACE_BEGIN

// 调色板长度 P = min(skinData 骨骼数, skin 世界矩阵数)。顶点骨骼索引是 skin 骨骼
// 数组的**全局下标**，故调色板按全局下标空间构建，P 为其有效长度。
// 唯一权威实现，供蒙皮收集（校验上界/守卫）与 mask 绘制（调色板构建/守卫）共用。
[[nodiscard]] uint32_t palette_slot_count(RE::NiSkinInstance const* a_skin);

// 蒙皮路径放弃/失败出口的定位日志——按 (节点名, 原因) 签名一次性输出（附数值），
// 同签名不重复。mask 子系统内共用。
void log_skinned_skip_once(bool a_warn, char const* a_node_name, std::string_view a_reason, std::string_view a_details);

// 从 mask 目标收集本帧几何 draw：静态（BSTriShape 家族）与蒙皮（NiSkinPartition
// 分区）两条路径，含位置格式/蒙皮布局的自标定与逐网格校验——无解的 draw 跳过
//（宁可少画不许垃圾涂屏）。目标列表顺序即尸体索引，超出槽位的目标 clamp 到
// 最后一个索引。
void collect_mask_draws(std::vector<MaskTarget> const& a_targets, std::vector<MaskDraw>& a_draws);

MASK_NAMESPACE_END
