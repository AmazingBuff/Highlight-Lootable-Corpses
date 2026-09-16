//
// Created by AmazingBuff on 2026/9/13.
//

#include "mask_geometry.h"

#include <DirectXMath.h>
#include <DirectXPackedVector.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_set>

MASK_NAMESPACE_BEGIN

void log_skinned_skip_once(bool a_warn, char const* a_node_name, std::string_view a_reason, std::string_view a_details);

namespace
{
    // ---- 静态 draw 足迹校验常量：世界包围球 / 目标归属 ----
    constexpr float Max_Part_World_Radius = 1024.0f;  // 世界包围球半径上限（游戏单位）
    constexpr float Ref_Proximity_Slack = 256.0f;     // draw 足迹到目标 ref 位置的最小邻域（游戏单位）

    constexpr std::size_t Max_Draws_Per_Frame = 256;
    constexpr std::uint32_t Max_Corpse_Index = 255;  // mask B 通道可精确存储的索引上限

    constexpr std::size_t Max_Position_Calibrations = 256;   // 标定缓存上限（超出退化不缓存）
    constexpr std::uint32_t Calibration_Sample_Limit = 256;  // 大网格采样上限

    constexpr std::size_t Max_Skinned_Layout_Calibrations = 256;  // 蒙皮标定缓存上限（超出退化不缓存）
    constexpr std::size_t Max_Skinned_Layout_Candidates = 4;      // 每步进最多 2 个 SKINNING 布局 × 2 步进

    // 权重校验阈值（SSE 顶点权重和约定为 1，阈值已刻意放宽）
    constexpr float Skinned_Weight_Min = -0.001f;
    constexpr float Skinned_Weight_Max = 1.001f;
    constexpr float Skinned_Weight_Sum_Min = 0.98f;
    constexpr float Skinned_Weight_Sum_Max = 1.02f;

    // ---------------------------------------------------------------------------
    // 一次性定位日志：按 key 去重（同 key 只输出一次）。签名容器只被渲染线程访问
    //（Present 回调内），仅首次命中各签名时增长。
    // ---------------------------------------------------------------------------
    void log_once(bool a_warn, std::string a_key, std::string_view a_message)
    {
        static std::unordered_set<std::string> s_signatures;
        if (s_signatures.emplace(std::move(a_key)).second)
        {
            if (a_warn)
                logger::warn("{}", a_message);
            else
                logger::info("{}", a_message);
        }
    }

    void log_static_skip_once(RE::FormID a_form_id, char const* a_node_name, std::string_view a_reason, std::string_view a_details)
    {
        char const* const node = a_node_name ? a_node_name : "?";
        log_once(false,
            fmt::format("static|{:08X}|{}|{}", a_form_id, node, a_reason),
            fmt::format("outline mask: skip static draw [{}] target={:08X} node=\"{}\" {}", a_reason, a_form_id, node, a_details));
    }

    void log_skinned_info_once(char const* a_node_name, std::string_view a_reason, std::string_view a_details)
    {
        char const* const node = a_node_name ? a_node_name : "?";
        log_once(false,
            fmt::format("skinned-info|{}|{}", node, a_reason),
            fmt::format("outline mask: skinned draw [{}] node=\"{}\" {}", a_reason, node, a_details));
    }

    void log_skinned_summary_once(char const* a_node_name, std::string_view a_details)
    {
        char const* const node = a_node_name ? a_node_name : "?";
        log_once(false,
            fmt::format("skinned-summary|{}", node),
            fmt::format("outline mask: skinned collect summary node=\"{}\" {}", node, a_details));
    }

    // ---------------------------------------------------------------------------
    // 顶点布局：由 vertexDesc 推导步进与属性格式。
    // CLibNG 的 VertexDesc::GetSize() 对半精度位置固定按 16 字节计（仅全精度成立）、
    // UV 固定按 4 字节计（全精度为 8），与真实步进不符；改为"各属性 offset+size
    // 取最大值"——offset 直接来自引擎写入的 desc（权威布局），属性大小为 SSE
    // 固定格式：
    //   POSITION   FULLPREC ? R32G32B32(12) : R16G16B16A16(8)
    //   TEXCOORDn  FULLPREC ? R32G32(8)     : R16G16(4)
    //   NORMAL/TANGENT/COLOR/EYEDATA/LANDDATA 4 字节
    //   SKINNING   权重 4×f16(8) + 索引 4×u8(4)，权重在前
    // SKINNING 块字节数可参数化：蒙皮分区步进未知，需按 8/12 两值试探；
    // 静态路径固定传 12。
    // ---------------------------------------------------------------------------
    std::uint32_t vertex_size_of_with_skinning(RE::BSGraphics::VertexDesc const& a_desc, std::uint32_t a_skinning_bytes)
    {
        bool const full = a_desc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC);
        std::uint32_t size = 0;
        auto const consider = [&](RE::BSGraphics::Vertex::Attribute a_attr, std::uint32_t a_bytes) {
            size = std::max(size, a_desc.GetAttributeOffset(a_attr) + a_bytes);
        };

        if (a_desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX))
            consider(RE::BSGraphics::Vertex::VA_POSITION, full ? 12u : 8u);
        if (a_desc.HasFlag(RE::BSGraphics::Vertex::VF_UV))
            consider(RE::BSGraphics::Vertex::VA_TEXCOORD0, full ? 8u : 4u);
        if (a_desc.HasFlag(RE::BSGraphics::Vertex::VF_UV_2))
            consider(RE::BSGraphics::Vertex::VA_TEXCOORD1, full ? 8u : 4u);
        if (a_desc.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL))
            consider(RE::BSGraphics::Vertex::VA_NORMAL, 4u);
        if (a_desc.HasFlag(RE::BSGraphics::Vertex::VF_TANGENT))
            consider(RE::BSGraphics::Vertex::VA_BINORMAL, 4u);
        if (a_desc.HasFlag(RE::BSGraphics::Vertex::VF_COLORS))
            consider(RE::BSGraphics::Vertex::VA_COLOR, 4u);
        if (a_desc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED))
            consider(RE::BSGraphics::Vertex::VA_SKINNING, a_skinning_bytes);
        if (a_desc.HasFlag(RE::BSGraphics::Vertex::VF_LANDDATA))
            consider(RE::BSGraphics::Vertex::VA_LANDDATA, 4u);
        if (a_desc.HasFlag(RE::BSGraphics::Vertex::VF_EYEDATA))
            consider(RE::BSGraphics::Vertex::VA_EYEDATA, 4u);

        return size;
    }

    std::uint32_t vertex_size_of(RE::BSGraphics::VertexDesc const& a_desc)
    {
        return vertex_size_of_with_skinning(a_desc, 12u);
    }

    DXGI_FORMAT position_format_of(RE::BSGraphics::VertexDesc const& a_desc)
    {
        return a_desc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC) ? DXGI_FORMAT_R32G32B32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
    }

    bool is_finite(RE::NiPoint3 const& a_p)
    {
        return std::isfinite(a_p.x) && std::isfinite(a_p.y) && std::isfinite(a_p.z);
    }

    // XMFLOAT4X4（世界变换）变换 3D 点：列向量消费，含平移（约定见 mask_types.h 矩阵工具注释）。
    // XMVector3Transform 求的是行向量积 p·M，故先转置，等价于 M·p。
    RE::NiPoint3 transform_point(DirectX::XMFLOAT4X4 const& a_m, RE::NiPoint3 const& a_p)
    {
        DirectX::XMMATRIX const transposed = DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&a_m));
        DirectX::XMFLOAT4 const point{ a_p.x, a_p.y, a_p.z, 1.0f };
        DirectX::XMFLOAT4 transformed{};
        DirectX::XMStoreFloat4(&transformed, DirectX::XMVector3Transform(DirectX::XMLoadFloat4(&point), transposed));
        return RE::NiPoint3{ transformed.x, transformed.y, transformed.z };
    }

    float distance_to_point(RE::NiPoint3 const& a_a, RE::NiPoint3 const& a_b)
    {
        float const dx = a_a.x - a_b.x;
        float const dy = a_a.y - a_b.y;
        float const dz = a_a.z - a_b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // ---------------------------------------------------------------------------
    // 位置属性格式标定：desc 的 VF_FULLPREC 不可靠（0x1b/0x3b 未置位，但位置实为
    // float32 全精度 16 字节槽位——按 half4 声明会让 D3D11 把每个 float32 的前
    // 8 字节当 4 个 half 读，位置全乱、三角形被拉到整屏）。故不再假定格式，而是
    // 用 rawVertexData 实测解码，与引擎 modelBound（模型空间地面真值）比对，选出
    // 吻合的 (格式, 字节偏移)，按 desc 缓存。
    // ---------------------------------------------------------------------------

    struct PositionCandidate
    {
        DXGI_FORMAT format;
        std::uint32_t bytes;
        bool from_desc;  // true：偏移取 desc 的 VA_POSITION；false：偏移 0
    };

    // 候选表（float32 在前：同分优先保守的全精度读法；集中定义，不散落魔法数）
    constexpr PositionCandidate Position_Candidates[] = {
        { DXGI_FORMAT_R32G32B32_FLOAT, 12, true },
        { DXGI_FORMAT_R32G32B32_FLOAT, 12, false },
        { DXGI_FORMAT_R16G16B16A16_FLOAT, 8, true },
        { DXGI_FORMAT_R16G16B16A16_FLOAT, 8, false },
    };
    constexpr std::size_t Position_Candidate_Count = sizeof(Position_Candidates) / sizeof(Position_Candidates[0]);

    enum class PositionCalibrationState
    {
        kMeasured,      // 实测通过，使用标定结果
        kDescFallback,  // 无法标定（raw 缺失等）→ 退回 desc 推导，不跳过
        kUnresolved,    // 无候选吻合 → 跳过该 draw（宁可少画不许垃圾涂屏）
    };

    struct PositionCalibration
    {
        // 仅布局决策（format/offset 为 desc 的属性，故可按 desc 缓存）。
        // 逐 mesh 的模型 AABB 只服务候选评分，不进缓存（防跨网格污染）。
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        std::uint32_t offset = 0;
        PositionCalibrationState state = PositionCalibrationState::kDescFallback;
    };

    // 单点位置解码：调用方须已保证 offset + bytes <= stride（防越界）
    bool decode_position(
        std::uint8_t const* a_base, std::uint32_t a_stride, std::uint32_t a_offset,
        std::uint32_t a_bytes, std::uint32_t a_index, RE::NiPoint3& a_out)
    {
        std::uint8_t const* src = a_base + static_cast<std::size_t>(a_index) * a_stride + a_offset;
        if (a_bytes == 12)
        {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            std::memcpy(&x, src + 0, sizeof(float));
            std::memcpy(&y, src + 4, sizeof(float));
            std::memcpy(&z, src + 8, sizeof(float));
            a_out = RE::NiPoint3{ x, y, z };
        }
        else
        {
            std::uint16_t half[4]{};
            std::memcpy(half, src, sizeof(half));
            a_out = RE::NiPoint3{
                DirectX::PackedVector::XMConvertHalfToFloat(half[0]),
                DirectX::PackedVector::XMConvertHalfToFloat(half[1]),
                DirectX::PackedVector::XMConvertHalfToFloat(half[2]),
            };
        }
        return is_finite(a_out);
    }

    // 采样解码求模型空间 AABB 的中心/半径与 min/max（采样上限摊平大网格）；任一非有限 → 失败
    bool measure_position(
        std::uint8_t const* a_base, std::uint32_t a_stride, std::uint32_t a_offset,
        std::uint32_t a_bytes, std::uint32_t a_vertex_count,
        RE::NiPoint3& a_center, float& a_radius, RE::NiPoint3& a_min, RE::NiPoint3& a_max)
    {
        std::uint32_t const step = (a_vertex_count > Calibration_Sample_Limit)
                                       ? (a_vertex_count + Calibration_Sample_Limit - 1) / Calibration_Sample_Limit
                                       : 1;
        RE::NiPoint3 min_p{};
        RE::NiPoint3 max_p{};
        bool first = true;
        for (std::uint32_t v = 0; v < a_vertex_count; v += step)
        {
            RE::NiPoint3 p{};
            if (!decode_position(a_base, a_stride, a_offset, a_bytes, v, p))
                return false;
            if (first)
            {
                min_p = max_p = p;
                first = false;
            }
            else
            {
                min_p.x = std::min(min_p.x, p.x);
                max_p.x = std::max(max_p.x, p.x);
                min_p.y = std::min(min_p.y, p.y);
                max_p.y = std::max(max_p.y, p.y);
                min_p.z = std::min(min_p.z, p.z);
                max_p.z = std::max(max_p.z, p.z);
            }
        }
        if (first)
            return false;

        a_center = RE::NiPoint3{ (min_p.x + max_p.x) * 0.5f, (min_p.y + max_p.y) * 0.5f, (min_p.z + max_p.z) * 0.5f };
        float const dx = max_p.x - min_p.x;
        float const dy = max_p.y - min_p.y;
        float const dz = max_p.z - min_p.z;
        a_radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
        a_min = min_p;
        a_max = max_p;
        return true;
    }

    // 判定并选定位置布局；每个 desc 首次标定输出一条完整证据日志（INFO 成功 /
    // WARN 无吻合）；退化路径一次性 INFO。结果按 desc 缓存（稳态零解码）。
    PositionCalibration calibrate_position_format(
        RE::BSGraphics::VertexDesc const& a_desc,
        RE::BSGraphics::TriShape const* a_renderer_data,
        std::uint32_t a_vertex_count,
        RE::NiBound const& a_model_bound,
        std::uint32_t a_stride)
    {
        std::uint64_t desc_raw = 0;
        std::memcpy(&desc_raw, &a_desc, sizeof(desc_raw));

        static std::vector<std::pair<std::uint64_t, PositionCalibration>> s_calibrations;
        for (auto const& [cached_raw, cached] : s_calibrations)
        {
            if (cached_raw == desc_raw)
                return cached;
        }

        std::uint32_t const desc_offset = a_desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_POSITION);
        bool const decodable = a_renderer_data && a_renderer_data->rawVertexData &&
                               a_desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) && a_stride > 0;

        PositionCalibration result{};
        std::string const desc_fields = fmt::format(
            "desc={:#018x} flags={:#06x} stride={} pos={} uv={} nrm={} bin={} col={} verts={} model_bound=({:.1f},{:.1f},{:.1f}) r={:.1f}",
            desc_raw,
            static_cast<unsigned>(a_desc.GetFlags()),
            a_stride,
            desc_offset,
            a_desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0),
            a_desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_NORMAL),
            a_desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_BINORMAL),
            a_desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_COLOR),
            a_vertex_count,
            a_model_bound.center.x, a_model_bound.center.y, a_model_bound.center.z, a_model_bound.radius);

        if (!decodable)
        {
            // 无法标定 → 退回 desc 推导（当前行为），一次性 INFO，不跳过
            result.format = position_format_of(a_desc);
            result.offset = desc_offset;
            result.state = PositionCalibrationState::kDescFallback;
            log_once(false, fmt::format("calib-pos|{:018x}|fallback", desc_raw),
                fmt::format("outline mask: position calibration unavailable (raw vertex data missing), using desc-derived layout {} format={:#06x} offset={}",
                    desc_fields, static_cast<unsigned>(result.format), result.offset));
            if (s_calibrations.size() < Max_Position_Calibrations)
                s_calibrations.emplace_back(desc_raw, result);
            return result;
        }

        struct CandidateResult
        {
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            std::uint32_t offset = 0;
            bool valid = false;   // 通过候选剔除（stride/越界）
            bool passed = false;  // 与 modelBound 吻合
            RE::NiPoint3 center{};
            float radius = 0.0f;
            float center_error = 0.0f;
        };
        CandidateResult results[Position_Candidate_Count];

        float const tolerance = 0.5f * std::max(a_model_bound.radius, 1.0f);
        for (std::size_t i = 0; i < Position_Candidate_Count; ++i)
        {
            PositionCandidate const& cand = Position_Candidates[i];
            CandidateResult& res = results[i];
            res.format = cand.format;
            res.offset = cand.from_desc ? desc_offset : 0;
            // 剔除：stride 不足以容纳格式，或偏移+格式越出该顶点步进
            if (a_stride < cand.bytes || res.offset + cand.bytes > a_stride)
                continue;
            res.valid = true;

            RE::NiPoint3 center{};
            float radius = 0.0f;
            RE::NiPoint3 model_min{};
            RE::NiPoint3 model_max{};
            if (!measure_position(a_renderer_data->rawVertexData, a_stride, res.offset, cand.bytes, a_vertex_count, center, radius, model_min, model_max))
                continue;

            res.center = center;
            res.radius = radius;
            res.center_error = distance_to_point(center, a_model_bound.center);
            res.passed = res.center_error <= tolerance &&
                         radius >= 0.4f * a_model_bound.radius &&
                         radius <= 2.5f * a_model_bound.radius;
        }

        // 取通过者中中心误差最小者；同分优先 float32（保守：宁可多读字节也不误读半精度）
        int best = -1;
        for (std::size_t i = 0; i < Position_Candidate_Count; ++i)
        {
            if (!results[i].passed)
                continue;
            if (best < 0)
            {
                best = static_cast<int>(i);
                continue;
            }
            float const best_error = results[best].center_error;
            float const error = results[i].center_error;
            bool const is_float32 = results[i].format == DXGI_FORMAT_R32G32B32_FLOAT;
            bool const best_is_float32 = results[best].format == DXGI_FORMAT_R32G32B32_FLOAT;
            if (error < best_error - 1e-4f || (std::fabs(error - best_error) <= 1e-4f && is_float32 && !best_is_float32))
                best = static_cast<int>(i);
        }

        std::string table;
        for (std::size_t i = 0; i < Position_Candidate_Count; ++i)
        {
            CandidateResult const& r = results[i];
            table += fmt::format(" [{} fmt={:#06x} off={} {} center=({:.1f},{:.1f},{:.1f}) r={:.1f} err={:.1f}]",
                i, static_cast<unsigned>(r.format), r.offset,
                r.passed ? "PASS" : (r.valid ? "fail" : "rejected"),
                r.center.x, r.center.y, r.center.z, r.radius, r.center_error);
        }

        if (best < 0)
        {
            result.state = PositionCalibrationState::kUnresolved;
            log_once(true, fmt::format("calib-pos|{:018x}|unresolved", desc_raw),
                fmt::format("outline mask: position calibration found no matching candidate, draw skipped {} candidates:{}",
                    desc_fields, table));
        }
        else
        {
            result.format = results[best].format;
            result.offset = results[best].offset;
            result.state = PositionCalibrationState::kMeasured;
            log_once(false, fmt::format("calib-pos|{:018x}|measured", desc_raw),
                fmt::format("outline mask: position calibration selected fmt={:#06x} off={} (center err {:.2f}) {} candidates:{}",
                    static_cast<unsigned>(result.format), result.offset, results[best].center_error, desc_fields, table));
        }

        if (s_calibrations.size() < Max_Position_Calibrations)
            s_calibrations.emplace_back(desc_raw, result);
        return result;
    }

    // ---------------------------------------------------------------------------
    // 蒙皮分区顶点布局标定：蒙皮分区步进（SKINNING 块字节数）与位置布局均无权威
    // 定义可核，故与静态路径同法——用分区自带 rawVertexData 实测解码，与几何
    // modelBound（模型空间地面真值，覆盖网格全部顶点，分区是其子集）比对。
    // 位置格式由属性偏移间距判定（gap = 下一个更靠后属性的偏移 - 位置偏移）：
    // 证据：flags 0x5b/0x9/0x1b/0x3b 的位置后紧邻属性偏移均为 16，即位置占
    // 16 字节槽位（float32）。候选为 位置格式 × 位置偏移 × 步进 的三维组合。
    // 结果按分区 desc 原始值独立缓存（与静态 s_calibrations 分离，判据不同）；
    // 无候选通过或 rawVertexData 缺失 → kUnresolved，调用方跳过并告警。
    // ---------------------------------------------------------------------------

    enum class SkinnedCalibrationState
    {
        kMeasured,    // 实测通过，使用标定结果
        kUnresolved,  // 无候选通过/无法标定 → 跳过该 draw
    };

    // 布局决策：缓存只保存这些字段，不含任何逐网格校验结论
    struct SkinnedVertexLayout
    {
        DXGI_FORMAT position_format = DXGI_FORMAT_UNKNOWN;
        std::uint32_t position_offset = 0;
        std::uint32_t stride = 0;
        std::uint8_t layout_id = 0;  // SKINNING 布局编号（1..4）
        MaskSkinLayout skin{};
        SkinnedCalibrationState state = SkinnedCalibrationState::kUnresolved;
    };

    // SKINNING 布局候选（顺序即优先级；*_delta 为相对 VA_SKINNING 偏移的字节增量）
    struct SkinningLayoutSpec
    {
        std::uint8_t id;
        DXGI_FORMAT weight_format;
        std::uint32_t weight_bytes;
        std::uint32_t weight_delta;
        DXGI_FORMAT index_format;
        std::uint32_t index_delta;
    };

    // ①/② 用于可用字节 A>=12；③/④ 用于 A==8
    constexpr SkinningLayoutSpec Skinning_Layouts[] = {
        { 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, 0, DXGI_FORMAT_R8G8B8A8_UINT, 8 },
        { 2, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, 4, DXGI_FORMAT_R8G8B8A8_UINT, 0 },
        { 3, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 0, DXGI_FORMAT_R8G8B8A8_UINT, 4 },
        { 4, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, DXGI_FORMAT_R8G8B8A8_UINT, 0 },
    };
    constexpr std::size_t Skinning_Layout_Count = sizeof(Skinning_Layouts) / sizeof(Skinning_Layouts[0]);

    // 按布局编号取候选定义（编号来自缓存布局决策，必命中；防御性返回首项）
    SkinningLayoutSpec const* find_skinning_layout(std::uint8_t a_id)
    {
        for (std::size_t i = 0; i < Skinning_Layout_Count; ++i)
        {
            if (Skinning_Layouts[i].id == a_id)
                return &Skinning_Layouts[i];
        }
        return &Skinning_Layouts[0];
    }

    // 逐网格校验统计：对每个网格在自身数据上全顶点遍历得出；索引按全局骨骼下标
    // 解释，上界为调色板长度 P
    struct SkinnedMeshStats
    {
        bool position_finite = true;                    // 选定位置格式下全部顶点有限
        std::uint32_t index_min = 0;
        std::uint32_t index_max = 0;
        std::uint32_t out_of_range_index_count = 0;     // 含 index >= P 槽位的顶点数
        std::uint32_t out_of_range_weighted_count = 0;  // 其中（该槽位）权重非零的顶点数 → 拒绝
        std::uint32_t first_out_of_range_index = 0;     // 首个越界索引
        float first_out_of_range_weight = 0.0f;         // 其权重
        float weight_sum_min = 0.0f;
        float weight_sum_max = 0.0f;
        std::uint32_t bad_weight_vertices = 0;          // 存在权重分量越界的顶点数
    };

    // 逐网格校验结论：位置失败 → 跳过；权重失败 → 换候选；索引越界且非零权重由
    // 调用方据 stats 判定为跳过，索引越界但零权重照常绘制
    enum class SkinnedMeshVerdict
    {
        kOk,
        kPositionBad,  // 位置非有限 → 跳过该网格
        kWeightsBad,   // 权重和/分量不合 → 缓存布局不适用本网格，重新枚举候选
    };

    // 候选实测结果（布局决策 + 该网格上的校验统计，用于日志表）
    struct SkinnedLayoutCandidateResult
    {
        std::uint32_t stride = 0;
        std::uint8_t layout = 0;
        bool bounds_ok = false;  // 越界剔除（不读取）
        SkinnedMeshStats stats{};
        bool passed = false;     // 位置有限 且 权重合格（索引越界不影响通过）
    };

    // 单顶点权重四元组解码（R16G16B16A16_FLOAT / R8G8B8A8_UNORM；调用方已保证不越界）
    void decode_weights(DXGI_FORMAT a_format, std::uint8_t const* a_src, float (&a_out)[4])
    {
        if (a_format == DXGI_FORMAT_R16G16B16A16_FLOAT)
        {
            std::uint16_t half[4]{};
            std::memcpy(half, a_src, sizeof(half));
            for (int i = 0; i < 4; ++i)
                a_out[i] = DirectX::PackedVector::XMConvertHalfToFloat(half[i]);
        }
        else
        {
            std::uint8_t byte[4]{};
            std::memcpy(byte, a_src, sizeof(byte));
            for (int i = 0; i < 4; ++i)
                a_out[i] = static_cast<float>(byte[i]) / 255.0f;
        }
    }

    // 单顶点 4 个骨骼索引解码（R8G8B8A8_UINT；调用方已保证不越界）
    void decode_indices(std::uint8_t const* a_src, std::uint8_t (&a_out)[4])
    {
        std::memcpy(a_out, a_src, sizeof(a_out));
    }

    // 单顶点权重分量是否全部在允许范围内
    bool weight_components_ok(float const (&a_w)[4])
    {
        for (int i = 0; i < 4; ++i)
        {
            if (a_w[i] < Skinned_Weight_Min || a_w[i] > Skinned_Weight_Max)
                return false;
        }
        return true;
    }

    // 在给定网格自身数据上、按给定位置布局与 SKINNING 布局遍历**全部**顶点，填充统计
    // 并返回结论。索引按全局骨骼下标解释，越界上界为 a_index_bound（调色板长度 P）。
    // 调用方须已保证各读取落在步进内（bounds 过滤）。
    SkinnedMeshVerdict validate_skinned_mesh(
        std::uint8_t const* a_raw, std::uint32_t a_stride, std::uint32_t a_pos_offset, std::uint32_t a_pos_bytes,
        SkinningLayoutSpec const& a_spec, std::uint32_t a_weight_offset, std::uint32_t a_index_offset,
        std::uint32_t a_vertex_count, std::uint32_t a_index_bound, SkinnedMeshStats& a_stats)
    {
        a_stats = SkinnedMeshStats{};
        a_stats.index_min = 0xFFFFFFFFu;  // 以哨兵起步，逐索引取 min（顶点数 > 0 由调用方保证）
        bool first_sample = true;
        for (std::uint32_t v = 0; v < a_vertex_count; ++v)
        {
            RE::NiPoint3 p{};
            if (!decode_position(a_raw, a_stride, a_pos_offset, a_pos_bytes, v, p))
                a_stats.position_finite = false;

            std::uint8_t const* const base = a_raw + static_cast<std::size_t>(v) * a_stride;
            float w[4]{};
            decode_weights(a_spec.weight_format, base + a_weight_offset, w);
            std::uint8_t idx[4]{};
            decode_indices(base + a_index_offset, idx);

            float sum = 0.0f;
            for (int i = 0; i < 4; ++i)
                sum += w[i];
            if (first_sample)
            {
                a_stats.weight_sum_min = a_stats.weight_sum_max = sum;
                first_sample = false;
            }
            else
            {
                a_stats.weight_sum_min = std::min(a_stats.weight_sum_min, sum);
                a_stats.weight_sum_max = std::max(a_stats.weight_sum_max, sum);
            }
            if (!weight_components_ok(w))
                ++a_stats.bad_weight_vertices;

            bool vertex_out_of_range = false;
            bool vertex_weighted_out_of_range = false;
            for (int i = 0; i < 4; ++i)
            {
                std::uint32_t const index = idx[i];
                a_stats.index_min = std::min(a_stats.index_min, index);
                a_stats.index_max = std::max(a_stats.index_max, index);
                if (index >= a_index_bound)
                {
                    vertex_out_of_range = true;
                    if (w[i] != 0.0f)  // 该越界槽位带非零权重 → 无法正确渲染
                        vertex_weighted_out_of_range = true;
                }
            }
            if (vertex_out_of_range)
            {
                ++a_stats.out_of_range_index_count;
                if (a_stats.out_of_range_weighted_count == 0)
                {
                    // 记录首个"越界且带非零权重"的槽位（供跳过日志取证）
                    for (int i = 0; i < 4; ++i)
                    {
                        if (static_cast<std::uint32_t>(idx[i]) >= a_index_bound && w[i] != 0.0f)
                        {
                            a_stats.first_out_of_range_index = static_cast<std::uint32_t>(idx[i]);
                            a_stats.first_out_of_range_weight = w[i];
                            break;
                        }
                    }
                }
                if (vertex_weighted_out_of_range)
                    ++a_stats.out_of_range_weighted_count;
            }
        }

        if (!a_stats.position_finite)
            return SkinnedMeshVerdict::kPositionBad;
        bool const weights_ok = a_stats.bad_weight_vertices == 0 &&
                                a_stats.weight_sum_min >= Skinned_Weight_Sum_Min &&
                                a_stats.weight_sum_max <= Skinned_Weight_Sum_Max;
        return weights_ok ? SkinnedMeshVerdict::kOk : SkinnedMeshVerdict::kWeightsBad;
    }

    // 候选表文本（一次性路径构造；每项含 stride/布局/位置有限性/权重和/越界计数/索引）
    std::string format_skinned_candidate_table(
        SkinnedLayoutCandidateResult const (&a_candidates)[Max_Skinned_Layout_Candidates],
        MaskSkinLayout const (&a_skin)[Max_Skinned_Layout_Candidates],
        std::size_t a_count,
        std::uint32_t a_index_bound)
    {
        std::string table;
        for (std::size_t i = 0; i < a_count; ++i)
        {
            SkinnedLayoutCandidateResult const& c = a_candidates[i];
            SkinnedMeshStats const& s = c.stats;
            table += fmt::format(
                " [{} L{} stride={} w(fmt={:#06x},off={}) i(fmt={:#06x},off={}) {} pos_finite={} wsum=[{:.3f},{:.3f}] wbad={} imax={}/{} oob={}(w{})]",
                i, static_cast<unsigned>(c.layout), c.stride,
                static_cast<unsigned>(a_skin[i].weight_format), a_skin[i].weight_offset,
                static_cast<unsigned>(a_skin[i].index_format), a_skin[i].index_offset,
                c.passed ? "PASS" : (c.bounds_ok ? "fail" : "rejected"),
                s.position_finite, s.weight_sum_min, s.weight_sum_max, s.bad_weight_vertices,
                s.index_max, a_index_bound, s.out_of_range_index_count, s.out_of_range_weighted_count);
        }
        return table;
    }

    // 枚举 (步进, SKINNING 布局) 候选并在给定网格自身数据上逐候选校验（全顶点），
    // 结果写入 a_candidates/a_skin，返回候选数。不读写缓存。
    std::size_t enumerate_skinned_candidates(
        RE::BSGraphics::VertexDesc const& a_desc,
        RE::BSGraphics::TriShape const* a_renderer_data,
        std::uint32_t a_vertex_count,
        std::uint32_t a_index_bound,
        std::uint32_t a_pos_offset,
        std::uint32_t a_pos_bytes,
        std::uint32_t a_skin_offset,
        SkinnedLayoutCandidateResult (&a_candidates)[Max_Skinned_Layout_Candidates],
        MaskSkinLayout (&a_skin)[Max_Skinned_Layout_Candidates])
    {
        // 步进候选（SKINNING 8/12 字节，去重）
        std::uint32_t const stride_options[2] = {
            vertex_size_of_with_skinning(a_desc, 8u),
            vertex_size_of_with_skinning(a_desc, 12u),
        };
        std::uint32_t strides[2]{};
        std::size_t stride_count = 0;
        for (std::uint32_t const stride : stride_options)
        {
            bool duplicate = false;
            for (std::size_t i = 0; i < stride_count; ++i)
                duplicate = duplicate || strides[i] == stride;
            if (!duplicate)
                strides[stride_count++] = stride;
        }

        bool const raw_available = a_renderer_data && a_renderer_data->rawVertexData && a_vertex_count > 0;
        std::uint8_t const* const raw = raw_available ? a_renderer_data->rawVertexData : nullptr;
        std::size_t candidate_count = 0;
        if (!raw_available)
            return 0;

        for (std::size_t si = 0; si < stride_count; ++si)
        {
            std::uint32_t const stride = strides[si];
            std::uint32_t const available = (stride > a_skin_offset) ? (stride - a_skin_offset) : 0u;
            for (std::size_t li = 0; li < Skinning_Layout_Count; ++li)
            {
                if (candidate_count >= Max_Skinned_Layout_Candidates)
                    break;  // 防御：候选数组写满即停（当前枚举最多命中其容量）
                SkinningLayoutSpec const& spec = Skinning_Layouts[li];
                bool const eligible = (spec.id <= 2) ? (available >= 12u) : (available == 8u);
                if (!eligible)
                    continue;

                std::uint32_t const weight_offset = a_skin_offset + spec.weight_delta;
                std::uint32_t const index_offset = a_skin_offset + spec.index_delta;
                // 越界防护：位置/权重/索引读取均须落在步进内，否则该候选不可用（不读取）
                bool const bounds_ok = (a_pos_offset + a_pos_bytes <= stride) &&
                                       (weight_offset + spec.weight_bytes <= stride) &&
                                       (index_offset + 4u <= stride);

                SkinnedLayoutCandidateResult& cr = a_candidates[candidate_count];
                cr.stride = stride;
                cr.layout = spec.id;
                cr.bounds_ok = bounds_ok;
                a_skin[candidate_count] = MaskSkinLayout{ spec.weight_format, weight_offset, spec.index_format, index_offset };

                if (bounds_ok)
                {
                    SkinnedMeshVerdict const verdict = validate_skinned_mesh(
                        raw, stride, a_pos_offset, a_pos_bytes, spec, weight_offset, index_offset,
                        a_vertex_count, a_index_bound, cr.stats);
                    // 索引越界不影响候选通过（由 mask 绘制的完整调色板上传兜底）
                    cr.passed = verdict == SkinnedMeshVerdict::kOk;
                }

                ++candidate_count;
            }
        }
        return candidate_count;
    }

    SkinnedVertexLayout calibrate_skinned_layout(
        RE::BSGraphics::VertexDesc const& a_desc,
        RE::BSGraphics::TriShape const* a_renderer_data,
        std::uint32_t a_vertex_count,
        std::uint32_t a_index_bound)
    {
        std::uint64_t desc_raw = 0;
        std::memcpy(&desc_raw, &a_desc, sizeof(desc_raw));

        static std::vector<std::pair<std::uint64_t, SkinnedVertexLayout>> s_skinned_calibrations;
        for (auto const& [cached_raw, cached] : s_skinned_calibrations)
        {
            if (cached_raw == desc_raw)
                return cached;
        }

        using V = RE::BSGraphics::Vertex;
        auto const attr_offset = [&a_desc](V::Attribute a_attr) {
            return a_desc.GetAttributeOffset(a_attr);
        };
        std::uint32_t const pos_offset = attr_offset(V::VA_POSITION);
        std::uint32_t const skin_offset = attr_offset(V::VA_SKINNING);

        // ---- 位置格式由属性偏移间距判定（gap = 下一个更靠后属性的偏移 - 位置偏移）。
        // 证据：static 标定与蒙皮标定日志中 flags 0x5b/0x9/0x1b/0x3b 的位置后紧邻
        // 属性偏移均为 16，即位置占 16 字节槽位（float32）。----
        std::uint32_t next_offset = 0;
        bool has_next = false;
        auto const consider_next = [&](bool a_present, std::uint32_t a_offset) {
            if (!a_present || a_offset <= pos_offset)
                return;
            if (!has_next || a_offset < next_offset)
            {
                next_offset = a_offset;
                has_next = true;
            }
        };
        consider_next(a_desc.HasFlag(V::VF_UV), attr_offset(V::VA_TEXCOORD0));
        consider_next(a_desc.HasFlag(V::VF_UV_2), attr_offset(V::VA_TEXCOORD1));
        consider_next(a_desc.HasFlag(V::VF_NORMAL), attr_offset(V::VA_NORMAL));
        consider_next(a_desc.HasFlag(V::VF_TANGENT), attr_offset(V::VA_BINORMAL));
        consider_next(a_desc.HasFlag(V::VF_COLORS), attr_offset(V::VA_COLOR));
        consider_next(a_desc.HasFlag(V::VF_SKINNED), attr_offset(V::VA_SKINNING));
        consider_next(a_desc.HasFlag(V::VF_LANDDATA), attr_offset(V::VA_LANDDATA));
        consider_next(a_desc.HasFlag(V::VF_EYEDATA), attr_offset(V::VA_EYEDATA));

        std::int32_t const gap = has_next ? (static_cast<std::int32_t>(next_offset) - static_cast<std::int32_t>(pos_offset)) : -1;
        bool const gap_degenerate = gap < 8;

        SkinnedVertexLayout result{};
        result.position_offset = pos_offset;
        if (gap >= 12)
            result.position_format = DXGI_FORMAT_R32G32B32_FLOAT;
        else if (gap >= 8)
            result.position_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        else
            result.position_format = position_format_of(a_desc);  // 退化：无更靠后属性 / gap 过小
        std::uint32_t const pos_bytes = (result.position_format == DXGI_FORMAT_R32G32B32_FLOAT) ? 12u : 8u;

        std::string const desc_fields = fmt::format(
            "desc={:#018x} flags={:#06x} pos={} uv={} nrm={} bin={} col={} skin={} gap={}{} pos_fmt={:#06x} pos_off={} verts={} index_bound={}",
            desc_raw,
            static_cast<unsigned>(a_desc.GetFlags()),
            pos_offset,
            attr_offset(V::VA_TEXCOORD0),
            attr_offset(V::VA_NORMAL),
            attr_offset(V::VA_BINORMAL),
            attr_offset(V::VA_COLOR),
            skin_offset,
            gap,
            gap_degenerate ? " (degenerate: no later attribute, fell back to fullprec flag)" : "",
            static_cast<unsigned>(result.position_format),
            pos_offset,
            a_vertex_count,
            a_index_bound);

        // ---- 候选枚举 + 逐候选在**本网格**数据上校验。全顶点遍历、不再抽样，
        // 且不读写缓存中的任何校验结论。----
        SkinnedLayoutCandidateResult candidates[Max_Skinned_Layout_Candidates];
        MaskSkinLayout candidate_skin[Max_Skinned_Layout_Candidates];
        std::size_t const candidate_count = enumerate_skinned_candidates(
            a_desc, a_renderer_data, a_vertex_count, a_index_bound, pos_offset, pos_bytes, skin_offset,
            candidates, candidate_skin);

        if (candidate_count == 0)
        {
            result.state = SkinnedCalibrationState::kUnresolved;
            log_once(true, fmt::format("calib-skin|{:018x}|unavailable", desc_raw),
                fmt::format("outline mask: skinned layout calibration unavailable ({}), draw skipped {}",
                    (a_renderer_data && a_renderer_data->rawVertexData) ? "geometry has no usable candidate" : "raw vertex data missing",
                    desc_fields));
            if (s_skinned_calibrations.size() < Max_Skinned_Layout_Calibrations)
                s_skinned_calibrations.emplace_back(desc_raw, result);
            return result;
        }

        // 取第一个全部通过的候选（优先级即枚举顺序：步进顺序 × 布局 1..4）
        std::int64_t best = -1;
        for (std::size_t i = 0; i < candidate_count; ++i)
        {
            if (candidates[i].passed)
            {
                best = static_cast<std::int64_t>(i);
                break;
            }
        }

        std::string const table = format_skinned_candidate_table(candidates, candidate_skin, candidate_count, a_index_bound);

        if (best < 0)
        {
            result.state = SkinnedCalibrationState::kUnresolved;
            log_once(true, fmt::format("calib-skin|{:018x}|unresolved", desc_raw),
                fmt::format("outline mask: skinned layout calibration found no matching candidate, draw skipped {} candidates:{}",
                    desc_fields, table));
        }
        else
        {
            result.stride = candidates[best].stride;
            result.layout_id = candidates[best].layout;
            result.skin = candidate_skin[best];
            result.state = SkinnedCalibrationState::kMeasured;
            log_once(false, fmt::format("calib-skin|{:018x}|measured", desc_raw),
                fmt::format("outline mask: skinned layout calibration selected pos(fmt={:#06x},off={}) stride={} layout={} weight(fmt={:#06x},off={}) index(fmt={:#06x},off={}) {} candidates:{}",
                    static_cast<unsigned>(result.position_format), result.position_offset,
                    result.stride, static_cast<unsigned>(result.layout_id),
                    static_cast<unsigned>(result.skin.weight_format), result.skin.weight_offset,
                    static_cast<unsigned>(result.skin.index_format), result.skin.index_offset,
                    desc_fields, table));
        }

        // 缓存只写入布局决策（result 结构内不含任何逐网格统计）
        if (s_skinned_calibrations.size() < Max_Skinned_Layout_Calibrations)
            s_skinned_calibrations.emplace_back(desc_raw, result);
        return result;
    }

    // ---------------------------------------------------------------------------
    // 几何收集
    // ---------------------------------------------------------------------------

    // 静态 draw 校验所需的目标上下文：位置与 form id 在 collect_draws 处按目标取
    // 一次，逐 geometry 复用。
    struct TargetContext
    {
        RE::NiPoint3 position{};
        RE::FormID form_id = 0;
        // 目标 3D 中含蒙皮几何 → 其非蒙皮几何（冰锥等装饰）不画。
        bool has_skinned = false;
        // 尸体索引（目标序号/255），mask PS 写入 B 通道供 LUT 查表。
        float corpse_index = 0.0f;
    };

    void collect_static(RE::BSGeometry* a_geom, RE::BSGeometry::GEOMETRY_RUNTIME_DATA const& a_geom_rt, TargetContext const& a_target, std::vector<MaskDraw>& a_draws)
    {
        // ---- 目标 3D 含蒙皮几何时，其非蒙皮几何（冰锥等装饰）不属于尸体本体，
        // 一律不画——剪影只反映蒙皮身体。纯静态目标（灰烬堆/静态尸体容器等）
        // has_skinned 为假，行为不变。----
        if (a_target.has_skinned)
        {
            char const* const node_name = a_geom->name.c_str();
            log_once(false,
                fmt::format("static-on-skinned|{}|{:08X}", node_name ? node_name : "?", a_target.form_id),
                fmt::format("outline mask: skip static geometry on skinned corpse (static prop is not part of the body silhouette) target={:08X} node=\"{}\"",
                    a_target.form_id, node_name ? node_name : "?"));
            return;
        }

        // ---- 静态路径专属的几何级 GPU 缓冲检查。蒙皮路径不走此门。----
        if (!a_geom_rt.rendererData || !a_geom_rt.rendererData->vertexBuffer || !a_geom_rt.rendererData->indexBuffer)
        {
            logger::debug("outline mask: skip geometry without GPU buffers");
            return;
        }

        // ---- 分类守卫：这些 BSTriShape 子类不能按普通静态几何绘制（动态顶点布局 /
        // 实例化第二顶点流 / 子范围索引结构），vertexDesc 数据标志同理——强行绘制
        // 会产生覆盖大半屏幕的垃圾三角形。原因一次性 INFO 记录。----
        char const* const rtti_name = a_geom->GetRTTI() ? a_geom->GetRTTI()->GetName() : "";
        char const* const node_name = a_geom->name.c_str();
        char const* exclusion_reason = nullptr;

        if (strcmp(rtti_name, "BSDynamicTriShape") == 0)
            exclusion_reason = "BSDynamicTriShape (dynamic vertex layout)";
        else if (strcmp(rtti_name, "BSInstanceTriShape") == 0)
            exclusion_reason = "BSInstanceTriShape (instanced, per-instance stream not bound)";
        else if (strcmp(rtti_name, "BSMultiStreamInstanceTriShape") == 0)
            exclusion_reason = "BSMultiStreamInstanceTriShape (multi-stream instanced, extra streams not bound)";
        else if (strcmp(rtti_name, "BSSubIndexTriShape") == 0)
            exclusion_reason = "BSSubIndexTriShape (decal sub-range index structure)";
        else if (a_geom_rt.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_INSTANCEDATA))
            exclusion_reason = "vertexDesc VF_INSTANCEDATA";
        else if (a_geom_rt.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_EYEDATA))
            exclusion_reason = "vertexDesc VF_EYEDATA";
        else if (a_geom_rt.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_LANDDATA))
            exclusion_reason = "vertexDesc VF_LANDDATA";

        if (exclusion_reason)
        {
            log_once(false, fmt::format("static-exclusion|{}", exclusion_reason),
                fmt::format("outline mask: skip static geometry ({}) rtti={} node=\"{}\"", exclusion_reason, rtti_name, node_name ? node_name : "?"));
            return;
        }

        RE::BSTriShape* tri = a_geom->AsTriShape();
        if (!tri)
        {
            // SSE 场景中的常规几何均为 BSTriShape 家族，其余类型不做 mask
            logger::debug("outline mask: skip non-BSTriShape geometry");
            return;
        }

        auto const& tri_rt = tri->GetTrishapeRuntimeData();
        if (tri_rt.vertexCount == 0 || tri_rt.triangleCount == 0)
        {
            log_once(false, "static|empty-geometry",
                fmt::format("outline mask: skip empty geometry (verts={} tris={}) rtti={} node=\"{}\"",
                    tri_rt.vertexCount, tri_rt.triangleCount, rtti_name, node_name ? node_name : "?"));
            return;
        }

        RE::NiBound const& model_bound = a_geom->GetModelData().modelBound;
        if (model_bound.radius <= 0.0f)
        {
            log_once(false, "static|invalid-model-bound",
                fmt::format("outline mask: skip geometry with invalid model bound rtti={} node=\"{}\"", rtti_name, node_name ? node_name : "?"));
            return;
        }

        std::uint32_t const vertex_stride = vertex_size_of(a_geom_rt.vertexDesc);

        // ---- 世界包围球校验。modelBound 经节点世界变换外推，3×3 各列范数最大值
        // 作为各向异性缩放上界；非有限/退化/超预算的世界球说明该网格不适合按普通
        // 静态几何画进 mask（效果类/异常数据）。----
        RE::NiTransform const& world_transform = a_geom->world;
        DirectX::XMFLOAT4X4 world{};
        DirectX::XMStoreFloat4x4(&world, DirectX::XMMatrixIdentity());
        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 3; ++col)
                world.m[row][col] = world_transform.rotate.entry[row][col] * world_transform.scale;
            world.m[row][3] = world_transform.translate[row];
        }
        float scale_max = 0.0f;
        for (int col = 0; col < 3; ++col)
        {
            float const dx = world.m[0][col];
            float const dy = world.m[1][col];
            float const dz = world.m[2][col];
            scale_max = std::max(scale_max, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        RE::NiPoint3 const world_center = transform_point(world, model_bound.center);
        float const world_radius = model_bound.radius * scale_max;
        if (!is_finite(world_center) || !std::isfinite(world_radius))
        {
            log_static_skip_once(a_target.form_id, node_name, "world bound non-finite",
                fmt::format("model_bound=({:.1f},{:.1f},{:.1f}) r={:.1f} world_center=({:.1f},{:.1f},{:.1f}) world_radius={:.1f}",
                    model_bound.center.x, model_bound.center.y, model_bound.center.z, model_bound.radius,
                    world_center.x, world_center.y, world_center.z, world_radius));
            return;
        }
        if (world_radius <= 0.0f || world_radius > Max_Part_World_Radius)
        {
            log_static_skip_once(a_target.form_id, node_name, "world radius out of range",
                fmt::format("model_bound r={:.1f} world_radius={:.1f} cap={:.1f}", model_bound.radius, world_radius, Max_Part_World_Radius));
            return;
        }

        // ---- 位置格式标定：不假定 VF_FULLPREC，按 rawVertexData 实测与 modelBound
        // 比对，选出格式+偏移并按 desc 缓存（稳态零解码）；无候选吻合 → 跳过该
        // draw（宁可少画不许垃圾涂屏）；无法标定 → 退回 desc 推导。----
        PositionCalibration const calibration = calibrate_position_format(
            a_geom_rt.vertexDesc, a_geom_rt.rendererData, tri_rt.vertexCount, model_bound, vertex_stride);
        if (calibration.state == PositionCalibrationState::kUnresolved)
            return;

        // ---- 归属校验——mask 只画"在目标处"的几何。统一使用引擎元数据世界包围球
        //（比盒判定更宽松；实测零误跳）。逐 mesh 模型 AABB 只服务标定的候选评分，
        // 不进缓存、不参与归属判定（防跨网格污染：同一 desc 的不同网格必须用自身
        // 数据判定）。----
        float const ref_distance = distance_to_point(a_target.position, world_center);
        float const proximity_limit = world_radius + Ref_Proximity_Slack;
        if (ref_distance > proximity_limit)
        {
            log_static_skip_once(a_target.form_id, node_name, "geometry not at target",
                fmt::format("distance={:.1f} limit={:.1f} ref=({:.1f},{:.1f},{:.1f}) world_center=({:.1f},{:.1f},{:.1f}) world_radius={:.1f}",
                    ref_distance, proximity_limit,
                    a_target.position.x, a_target.position.y, a_target.position.z,
                    world_center.x, world_center.y, world_center.z, world_radius));
            return;
        }

        MaskDraw draw{};
        draw.vertex_buffer = reinterpret_cast<ID3D11Buffer*>(a_geom_rt.rendererData->vertexBuffer);
        draw.index_buffer = reinterpret_cast<ID3D11Buffer*>(a_geom_rt.rendererData->indexBuffer);
        draw.vertex_desc = a_geom_rt.vertexDesc;
        draw.node = a_geom;
        draw.node_ref.reset(a_geom);  // 保活：几何体被卸载时 node/rendererData/VB/IB 仍有效到本帧结束
        draw.vertex_stride = vertex_stride;
        draw.vertex_count = tri_rt.vertexCount;
        draw.triangle_count = tri_rt.triangleCount;
        draw.index_count = static_cast<std::uint32_t>(tri_rt.triangleCount) * 3u;
        draw.position_format = calibration.format;
        draw.position_offset = calibration.offset;
        draw.corpse_index = a_target.corpse_index;
        a_draws.push_back(std::move(draw));
    }

    void collect_skinned(RE::BSGeometry* a_geom, RE::BSGeometry::GEOMETRY_RUNTIME_DATA const& a_geom_rt, TargetContext const& a_target, std::vector<MaskDraw>& a_draws)
    {
        char const* const node_name = a_geom->name.c_str();
        char const* const rtti_name = a_geom->GetRTTI() ? a_geom->GetRTTI()->GetName() : "?";

        std::size_t const draws_before = a_draws.size();

        RE::NiSkinInstance* skin = a_geom_rt.skinInstance.get();
        RE::NiSkinPartition* skin_partition = skin ? skin->skinPartition.get() : nullptr;
        if (!skin || !skin_partition || !skin->skinData || !skin->skinData->GetBoneData() || !skin->boneWorldTransforms || !skin->bones)
        {
            // 指名哪些字段为空
            std::string missing;
            auto const note = [&missing](bool a_null, char const* a_field) {
                if (a_null)
                {
                    if (!missing.empty())
                        missing += ", ";
                    missing += a_field;
                }
            };
            note(!skin, "skinInstance");
            note(skin && !skin_partition, "skinPartition");
            note(skin && !skin->skinData, "skinData");
            note(skin && skin->skinData && !skin->skinData->GetBoneData(), "skinData->GetBoneData()");
            note(skin && !skin->boneWorldTransforms, "boneWorldTransforms");
            note(skin && !skin->bones, "bones");
            log_skinned_skip_once(true, node_name, "skin instance incomplete",
                fmt::format("missing=[{}] rtti={}", missing, rtti_name ? rtti_name : "?"));
            log_skinned_summary_once(node_name, fmt::format("skinned draws added={} (incomplete skin instance)", a_draws.size() - draws_before));
            return;
        }

        // 蒙皮按分区绘制：分区自带重映射顶点缓冲（buffData），调色板每分区一份。
        // 分区数以 numPartitions 与分区数组实际容量中较小者为准（防损坏数据越界）。
        std::uint32_t const partition_count = std::min<std::uint32_t>(
            skin_partition->numPartitions, static_cast<std::uint32_t>(skin_partition->partitions.size()));
        if (partition_count < skin_partition->numPartitions)
        {
            log_skinned_skip_once(false, node_name, "partition count exceeds array size",
                fmt::format("numPartitions={} partitions.size()={} extra partitions ignored",
                    skin_partition->numPartitions, skin_partition->partitions.size()));
        }
        if (partition_count == 0)
        {
            log_skinned_skip_once(false, node_name, "no skin partitions",
                fmt::format("numPartitions={} partitions.size()={}", skin_partition->numPartitions, skin_partition->partitions.size()));
            log_skinned_summary_once(node_name, fmt::format("skinned draws added=0 partitions={}", partition_count));
            return;
        }

        RE::NiAVObject* const root_parent = skin->rootParent;
        if (!root_parent)
        {
            log_skinned_skip_once(true, node_name, "skin instance has no rootParent",
                fmt::format("rtti={} partitions={}", rtti_name ? rtti_name : "?", partition_count));
            log_skinned_summary_once(node_name, fmt::format("skinned draws added={} (no rootParent)", a_draws.size() - draws_before));
            return;
        }

        // ---- 弃用 modelBound 前提。引擎管理的蒙皮网格 modelBound 实为 0（实测：
        // 全部身体/装备/毛发网格 r=0.0），不得据此跳过。只做一次性 INFO 记录；
        // 包围球仅在引擎确实提供（worldBound.radius > 0）时用作廉价 sanity gate
        //——非有限或超预算才跳过（一次性 WARN）。----
        RE::NiBound const& model_bound = a_geom->GetModelData().modelBound;
        RE::NiBound const& world_bound = a_geom->worldBound;
        log_skinned_info_once(node_name, "world bound reported",
            fmt::format("model_bound=({:.1f},{:.1f},{:.1f}) r={:.1f} world_bound=({:.1f},{:.1f},{:.1f}) r={:.1f}",
                model_bound.center.x, model_bound.center.y, model_bound.center.z, model_bound.radius,
                world_bound.center.x, world_bound.center.y, world_bound.center.z, world_bound.radius));

        if (world_bound.radius > 0.0f)
        {
            bool const finite = is_finite(world_bound.center) && std::isfinite(world_bound.radius);
            if (!finite || world_bound.radius > Max_Part_World_Radius)
            {
                log_skinned_skip_once(true, node_name, "world bound out of range",
                    fmt::format("world_bound=({:.1f},{:.1f},{:.1f}) r={:.1f} cap={:.1f} finite={}",
                        world_bound.center.x, world_bound.center.y, world_bound.center.z, world_bound.radius,
                        Max_Part_World_Radius, finite));
                log_skinned_summary_once(node_name, fmt::format("skinned draws added={} (world bound out of range)", a_draws.size() - draws_before));
                return;
            }
        }

        for (std::uint32_t p = 0; p < partition_count; ++p)
        {
            RE::NiSkinPartition::Partition const& part = skin_partition->partitions[p];
            RE::BSGraphics::TriShape* const buff = part.buffData;

            // 分区被拒——指名具体条件 + 分区序号 + vertices/triangles
            char const* reject = nullptr;
            if (!buff)
                reject = "partition buffData missing";
            else if (!buff->vertexBuffer)
                reject = "partition vertex buffer missing";
            else if (!buff->indexBuffer)
                reject = "partition index buffer missing";
            else if (part.triangles == 0)
                reject = "partition has no triangles";
            else if (!part.triList)
                reject = "partition triList missing";
            if (reject)
            {
                log_skinned_skip_once(false, node_name, reject,
                    fmt::format("partition={} vertices={} triangles={}", p, part.vertices, part.triangles));
                continue;
            }

            if (part.strips != 0)
            {
                // 条带分区（stripLengths 索引布局）不能按三角形列表绘制——跳过防错
                log_skinned_skip_once(false, node_name, "partition is a triangle strip",
                    fmt::format("partition={} strips={} vertices={} triangles={}", p, part.strips, part.vertices, part.triangles));
                continue;
            }

            // ---- 调色板按**全局骨骼索引空间**构建（顶点索引即 skin 骨骼数组下标：
            // 实测 numBones=10/index_max=61/skin_bones=61）。有效长度
            // P = min(GetBoneCount(), numMatrices)。part.bones/numBones 此后仅供诊断
            // 日志，不参与任何判定。----
            std::uint32_t const skin_bone_count = skin->skinData->GetBoneCount();
            std::uint32_t const matrix_count = skin->numMatrices;
            std::uint32_t const palette_count = palette_slot_count(skin);
            log_skinned_info_once(node_name, "bone arrays reported",
                fmt::format("partition={} numBones={} bones=[{},{},{}] skin_bones={} numMatrices={} numRegisters={} P={}",
                    p, part.numBones,
                    part.bones ? part.bones[0] : static_cast<std::uint16_t>(0),
                    (part.bones && part.numBones > 1) ? part.bones[1] : static_cast<std::uint16_t>(0),
                    (part.bones && part.numBones > 2) ? part.bones[2] : static_cast<std::uint16_t>(0),
                    skin_bone_count, matrix_count, skin->numRegisters, palette_count));
            if (palette_count == 0 || palette_count > Max_Palette_Bones)
            {
                log_skinned_skip_once(true, node_name, "palette slot count out of range",
                    fmt::format("partition={} P={} skin_bones={} numMatrices={} budget={}",
                        p, palette_count, skin_bone_count, matrix_count, Max_Palette_Bones));
                continue;
            }

            // ---- 位置格式按属性偏移间距判定；步进与 SKINNING（权重/索引）布局按
            // 网格自身顶点数据自标定；无解则跳过（不得退回 UNKNOWN 位置格式或硬编码
            // 蒙皮顺序）。----
            SkinnedVertexLayout calibration = calibrate_skinned_layout(
                buff->vertexDesc, buff, part.vertices, palette_count);
            if (calibration.state != SkinnedCalibrationState::kMeasured)
                continue;

            // ---- 逐网格校验**对本网格**执行（不复用他网格结论，全顶点遍历）；索引按
            // 全局下标、上界为 P。位置失败 → 跳过；权重不合 → 重新枚举候选（不写
            // 缓存）；索引越界且带非零权重 → 跳过；越界但该槽位权重为 0 → 照常绘制
            //（副本填充使其为无操作），仅在诊断中计数。----
            std::uint8_t const* const raw = buff->rawVertexData;
            std::uint32_t const pos_bytes = (calibration.position_format == DXGI_FORMAT_R32G32B32_FLOAT) ? 12u : 8u;
            SkinningLayoutSpec const* spec = find_skinning_layout(calibration.layout_id);

            SkinnedMeshStats stats{};
            SkinnedMeshVerdict verdict = validate_skinned_mesh(
                raw, calibration.stride, calibration.position_offset, pos_bytes, *spec,
                calibration.skin.weight_offset, calibration.skin.index_offset,
                part.vertices, palette_count, stats);

            if (verdict == SkinnedMeshVerdict::kPositionBad)
            {
                log_skinned_skip_once(true, node_name, "mesh positions non-finite",
                    fmt::format("partition={} verts={} stride={} pos(fmt={:#06x},off={})",
                        p, part.vertices, calibration.stride, static_cast<unsigned>(calibration.position_format), calibration.position_offset));
                continue;
            }
            if (verdict == SkinnedMeshVerdict::kWeightsBad)
            {
                // 缓存布局不适用本网格：在本网格数据上重新枚举候选（不写缓存）
                SkinnedLayoutCandidateResult candidates[Max_Skinned_Layout_Candidates];
                MaskSkinLayout candidate_skin[Max_Skinned_Layout_Candidates];
                std::uint32_t const skin_offset = buff->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING);
                std::size_t const candidate_count = enumerate_skinned_candidates(
                    buff->vertexDesc, buff, part.vertices, palette_count, calibration.position_offset, pos_bytes,
                    skin_offset, candidates, candidate_skin);
                std::int64_t switch_to = -1;
                for (std::size_t i = 0; i < candidate_count; ++i)
                {
                    if (candidates[i].passed)
                    {
                        switch_to = static_cast<std::int64_t>(i);
                        break;
                    }
                }
                if (switch_to < 0)
                {
                    log_skinned_skip_once(true, node_name, "no skinned layout fits this mesh",
                        fmt::format("partition={} verts={} P={} cached_layout={} candidates:{}",
                            p, part.vertices, palette_count, static_cast<unsigned>(calibration.layout_id),
                            format_skinned_candidate_table(candidates, candidate_skin, candidate_count, palette_count)));
                    continue;
                }
                calibration.stride = candidates[switch_to].stride;
                calibration.layout_id = candidates[switch_to].layout;
                calibration.skin = candidate_skin[switch_to];
                spec = find_skinning_layout(calibration.layout_id);
                verdict = validate_skinned_mesh(
                    raw, calibration.stride, calibration.position_offset, pos_bytes, *spec,
                    calibration.skin.weight_offset, calibration.skin.index_offset,
                    part.vertices, palette_count, stats);
                if (verdict != SkinnedMeshVerdict::kOk)
                {
                    log_skinned_skip_once(true, node_name, "switched layout still fails mesh validation",
                        fmt::format("partition={} verts={} P={} stride={} layout={}",
                            p, part.vertices, palette_count, calibration.stride, static_cast<unsigned>(calibration.layout_id)));
                    continue;
                }
                log_skinned_info_once(node_name, "skinned layout switched for mesh",
                    fmt::format("partition={} verts={} P={} stride={} layout={} w(fmt={:#06x},off={}) i(fmt={:#06x},off={})",
                        p, part.vertices, palette_count, calibration.stride, static_cast<unsigned>(calibration.layout_id),
                        static_cast<unsigned>(calibration.skin.weight_format), calibration.skin.weight_offset,
                        static_cast<unsigned>(calibration.skin.index_format), calibration.skin.index_offset));
            }

            // 每个蒙皮网格首次一条诊断 INFO（含 P/skin_bones/numMatrices 与越界计数）
            log_skinned_info_once(node_name, "mesh diagnostics",
                fmt::format("rtti={} verts={} numBones={} P={} skin_bones={} numMatrices={} index=[{},{}] oob={} oob_weighted={} wsum=[{:.3f},{:.3f}] wbad={} pos(fmt={:#06x},off={}) stride={} layout={}",
                    rtti_name ? rtti_name : "?", part.vertices, part.numBones, palette_count,
                    skin_bone_count, matrix_count,
                    stats.index_min, stats.index_max, stats.out_of_range_index_count, stats.out_of_range_weighted_count,
                    stats.weight_sum_min, stats.weight_sum_max, stats.bad_weight_vertices,
                    static_cast<unsigned>(calibration.position_format), calibration.position_offset,
                    calibration.stride, static_cast<unsigned>(calibration.layout_id)));

            // 索引 >= P 且**带非零权重** → 无法正确渲染，跳过 + WARN（整块副本填充只对
            // 零权重槽位是无操作，故零权重越界不在此列，仅在诊断中计数）。
            if (stats.out_of_range_weighted_count > 0)
            {
                log_skinned_skip_once(true, node_name, "bone index exceeds palette bounds with non-zero weight",
                    fmt::format("partition={} verts={} P={} skin_bones={} numMatrices={} oob={} oob_weighted={} index_max={} first_oob_index={} first_oob_weight={:.4f} numBones={} bones=[{},{},{}]",
                        p, part.vertices, palette_count, skin_bone_count, matrix_count,
                        stats.out_of_range_index_count, stats.out_of_range_weighted_count, stats.index_max,
                        stats.first_out_of_range_index, stats.first_out_of_range_weight,
                        part.numBones,
                        part.bones ? part.bones[0] : static_cast<std::uint16_t>(0),
                        (part.bones && part.numBones > 1) ? part.bones[1] : static_cast<std::uint16_t>(0),
                        (part.bones && part.numBones > 2) ? part.bones[2] : static_cast<std::uint16_t>(0)));
                continue;
            }

            MaskDraw draw{};
            draw.skinned = true;
            draw.skin = a_geom_rt.skinInstance;
            draw.partition = p;
            draw.node = a_geom;
            draw.node_ref.reset(a_geom);  // 保活几何体
            draw.vertex_buffer = reinterpret_cast<ID3D11Buffer*>(buff->vertexBuffer);
            draw.index_buffer = reinterpret_cast<ID3D11Buffer*>(buff->indexBuffer);
            draw.vertex_desc = buff->vertexDesc;
            draw.vertex_stride = calibration.stride;
            draw.vertex_count = part.vertices;
            draw.triangle_count = part.triangles;
            draw.index_count = static_cast<std::uint32_t>(part.triangles) * 3u;
            draw.position_format = calibration.position_format;
            draw.position_offset = calibration.position_offset;
            draw.skin_layout = calibration.skin;
            draw.corpse_index = a_target.corpse_index;
            a_draws.push_back(std::move(draw));
        }

        log_skinned_summary_once(node_name, fmt::format("skinned draws added={} partitions={}", a_draws.size() - draws_before, partition_count));
    }

    void collect_geometry(RE::BSGeometry* a_geom, TargetContext const& a_target, std::vector<MaskDraw>& a_draws)
    {
        // 粒子/线段类不画 mask（非目标形态）
        switch (a_geom->GetType().get())
        {
        case RE::BSGeometry::Type::kParticles:
        case RE::BSGeometry::Type::kStripParticles:
        case RE::BSGeometry::Type::kParticleShaderDynamicTriShape:
        case RE::BSGeometry::Type::kLines:
        case RE::BSGeometry::Type::kDynamicLines:
        case RE::BSGeometry::Type::kInstanceGroup:
            logger::debug("outline mask: skip particle/line geometry (type {})", static_cast<int>(a_geom->GetType().get()));
            return;
        default:
            break;
        }

        auto const& geom_rt = a_geom->GetGeometryRuntimeData();

        // ---- 效果着色器几何（BSEffectShaderProperty：发光 sprite/拖尾/粒子类 fx
        // 附件）不是实体表面，顶点数据也不保证按"模型空间实体表面"语义解释——整类
        // 排除。首次命中 INFO（含首个节点名），此后静默。----
        if (geom_rt.shaderProperty && geom_rt.shaderProperty->GetRTTI() &&
            strcmp(geom_rt.shaderProperty->GetRTTI()->GetName(), "BSEffectShaderProperty") == 0)
        {
            log_once(false, "static|effect-shader",
                fmt::format("outline mask: skip effect-shader geometry (BSEffectShaderProperty, fx attachment is not part of the corpse silhouette) node=\"{}\"",
                    a_geom->name.c_str() ? a_geom->name.c_str() : "?"));
            return;
        }

        // 几何级 GPU 缓冲检查是静态路径专属——蒙皮路径用 NiSkinPartition::Partition::
        // buffData（分区自带 VB/IB）。
        if (geom_rt.skinInstance)
        {
            collect_skinned(a_geom, geom_rt, a_target, a_draws);
            return;
        }
        collect_static(a_geom, geom_rt, a_target, a_draws);
    }
}

std::uint32_t palette_slot_count(RE::NiSkinInstance const* a_skin)
{
    if (!a_skin || !a_skin->skinData)
        return 0;
    std::uint32_t const skin_bones = a_skin->skinData->GetBoneCount();
    std::uint32_t const matrix_count = a_skin->numMatrices;
    return std::min(skin_bones, matrix_count);
}

void log_skinned_skip_once(bool a_warn, char const* a_node_name, std::string_view a_reason, std::string_view a_details)
{
    char const* const node = a_node_name ? a_node_name : "?";
    log_once(a_warn,
        fmt::format("skinned|{}|{}", node, a_reason),
        fmt::format("outline mask: skip skinned draw [{}] node=\"{}\" {}", a_reason, node, a_details));
}

void collect_mask_draws(std::vector<MaskTarget> const& a_targets, std::vector<MaskDraw>& a_draws)
{
    // 目标列表顺序即尸体索引（renderer 构造顺序 = corpses 快照顺序）；超出索引
    // 槽位的目标 clamp 到最后一个索引并一次性 WARN。
    static bool s_index_clamp_reported = false;
    if (a_targets.size() > Max_Corpse_Index + 1 && !s_index_clamp_reported)
    {
        s_index_clamp_reported = true;
        logger::warn("outline mask: {} targets exceed the {} corpse-index slots, extras share the last index",
            a_targets.size(), Max_Corpse_Index + 1);
    }

    for (std::size_t ti = 0; ti < a_targets.size(); ++ti)
    {
        MaskTarget const& target = a_targets[ti];
        std::size_t const corpse_index = std::min<std::size_t>(ti, Max_Corpse_Index);
        RE::TESObjectREFR* ref = target.ref.get();
        if (!ref)
            continue;

        RE::NiAVObject* root = ref->GetCurrent3D();
        if (!root)
            continue;  // 3D 未加载（未实例化），常态跳过

        // ---- 先做一次**短路**蒙皮探测（遇到任何蒙皮几何即停），供 collect_static
        // 判定是否剔除该目标的静态装饰。----
        bool has_skinned = false;
        RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geom) {
            if (a_geom->GetGeometryRuntimeData().skinInstance)
            {
                has_skinned = true;
                return RE::BSVisit::BSVisitControl::kStop;
            }
            return RE::BSVisit::BSVisitControl::kContinue;
        });

        TargetContext const target_ctx{
            .position = ref->GetPosition(), .form_id = ref->GetFormID(), .has_skinned = has_skinned,
            .corpse_index = static_cast<float>(corpse_index) / 255.0f };

        RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geom) {
            if (a_draws.size() >= Max_Draws_Per_Frame)
            {
                static bool s_draw_cap_reported = false;
                if (!s_draw_cap_reported)
                {
                    s_draw_cap_reported = true;
                    logger::warn("outline mask: draw cap {} reached, extra geometry dropped", Max_Draws_Per_Frame);
                }
                return RE::BSVisit::BSVisitControl::kStop;
            }
            collect_geometry(a_geom, target_ctx, a_draws);
            return RE::BSVisit::BSVisitControl::kContinue;
        });
    }
}

MASK_NAMESPACE_END
