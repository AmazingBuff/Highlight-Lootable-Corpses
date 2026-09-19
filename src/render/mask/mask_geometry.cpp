//
// Created by AmazingBuff on 2026/9/13.
//

#include "mask_geometry.h"

MASK_NAMESPACE_BEGIN

namespace
{
    // ---- Constants of the static draw footprint checks: world bounding sphere / target ownership ----
    constexpr float Max_Part_World_Radius = 1024.0f;  // upper bound on the world bounding-sphere radius (game units)
    constexpr float Ref_Proximity_Slack = 256.0f;     // minimum neighbourhood of the draw footprint around the target ref position (game units)

    constexpr size_t Max_Draws_Per_Frame = 256;

    constexpr size_t Max_Position_Calibrations = 256;   // calibration cache cap (past it, no caching on the degraded path)
    constexpr uint32_t Calibration_Sample_Limit = 256;  // sampling cap for large meshes

    constexpr size_t Max_Skinned_Layout_Calibrations = 256;  // skinned calibration cache cap (past it, no caching on the degraded path)
    constexpr size_t Max_Skinned_Layout_Candidates = 4;      // at most 2 SKINNING layouts per stride × 2 strides

    // Weight validation thresholds (SSE vertex weights are expected to sum to 1, so the thresholds are deliberately loose)
    constexpr float Skinned_Weight_Min = -0.001f;
    constexpr float Skinned_Weight_Max = 1.001f;
    constexpr float Skinned_Weight_Sum_Min = 0.98f;
    constexpr float Skinned_Weight_Sum_Max = 1.02f;

    // ---------------------------------------------------------------------------
    // Vertex layout: CLibNG's VertexDesc::GetSize() miscounts the half-precision position (16) and
    // UV (4), so the stride is derived as the max of offset+size over all attributes instead -
    // offsets come from the engine-written desc, sizes are the fixed SSE formats (position/UV are
    // FULLPREC-dependent, NORMAL/TANGENT/COLOR/EYEDATA/LANDDATA are 4 bytes, SKINNING is weights
    // 4×f16 + indices 4×u8 with weights first). The SKINNING byte count is parameterisable: the
    // skinned stride is unknown and both 8 and 12 are probed; the static path always passes 12.
    // ---------------------------------------------------------------------------
    uint32_t vertex_size_of_with_skinning(RE::BSGraphics::VertexDesc const& desc, uint32_t skinning_bytes)
    {
        bool const full = desc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC);
        uint32_t size = 0;
        auto const consider = [&](RE::BSGraphics::Vertex::Attribute attr, uint32_t bytes) {
            size = std::max(size, desc.GetAttributeOffset(attr) + bytes);
        };

        if (desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX))
            consider(RE::BSGraphics::Vertex::VA_POSITION, full ? 12u : 8u);
        if (desc.HasFlag(RE::BSGraphics::Vertex::VF_UV))
            consider(RE::BSGraphics::Vertex::VA_TEXCOORD0, full ? 8u : 4u);
        if (desc.HasFlag(RE::BSGraphics::Vertex::VF_UV_2))
            consider(RE::BSGraphics::Vertex::VA_TEXCOORD1, full ? 8u : 4u);
        if (desc.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL))
            consider(RE::BSGraphics::Vertex::VA_NORMAL, 4u);
        if (desc.HasFlag(RE::BSGraphics::Vertex::VF_TANGENT))
            consider(RE::BSGraphics::Vertex::VA_BINORMAL, 4u);
        if (desc.HasFlag(RE::BSGraphics::Vertex::VF_COLORS))
            consider(RE::BSGraphics::Vertex::VA_COLOR, 4u);
        if (desc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED))
            consider(RE::BSGraphics::Vertex::VA_SKINNING, skinning_bytes);
        if (desc.HasFlag(RE::BSGraphics::Vertex::VF_LANDDATA))
            consider(RE::BSGraphics::Vertex::VA_LANDDATA, 4u);
        if (desc.HasFlag(RE::BSGraphics::Vertex::VF_EYEDATA))
            consider(RE::BSGraphics::Vertex::VA_EYEDATA, 4u);

        return size;
    }

    uint32_t vertex_size_of(RE::BSGraphics::VertexDesc const& desc)
    {
        return vertex_size_of_with_skinning(desc, 12u);
    }

    REX::W32::DXGI_FORMAT position_format_of(RE::BSGraphics::VertexDesc const& desc)
    {
        return desc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC) ? REX::W32::DXGI_FORMAT_R32G32B32_FLOAT : REX::W32::DXGI_FORMAT_R16G16B16A16_FLOAT;
    }

    // Column-vector point transform M·p (translation included): XMVector3Transform computes p·M, so
    // transpose first.
    RE::NiPoint3 transform_point(DirectX::XMFLOAT4X4 const& m, RE::NiPoint3 const& p)
    {
        DirectX::XMMATRIX const transposed = DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&m));
        DirectX::XMFLOAT4 const point{ p.x, p.y, p.z, 1.0f };
        DirectX::XMFLOAT4 transformed{};
        DirectX::XMStoreFloat4(&transformed, DirectX::XMVector3Transform(DirectX::XMLoadFloat4(&point), transposed));
        return RE::NiPoint3{ transformed.x, transformed.y, transformed.z };
    }

    float distance_to_point(RE::NiPoint3 const& a, RE::NiPoint3 const& b)
    {
        float const dx = a.x - b.x;
        float const dy = a.y - b.y;
        float const dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // ---------------------------------------------------------------------------
    // Position format calibration: VF_FULLPREC is unreliable (0x1b/0x3b leave the flag clear on a
    // float32 position; misdeclaring it as half4 scrambles every position). Instead rawVertexData is
    // decoded and compared against the engine modelBound to select the matching (format, offset),
    // cached per desc.
    // ---------------------------------------------------------------------------

    struct PositionCandidate
    {
        REX::W32::DXGI_FORMAT format;
        uint32_t bytes;
        bool from_desc;  // true: offset taken from the desc's VA_POSITION; false: offset 0
    };

    // Candidate table (float32 first: on a tie prefer the conservative full-precision reading; defined in one place, no scattered magic numbers)
    constexpr PositionCandidate Position_Candidates[] = {
        { REX::W32::DXGI_FORMAT_R32G32B32_FLOAT, 12, true },
        { REX::W32::DXGI_FORMAT_R32G32B32_FLOAT, 12, false },
        { REX::W32::DXGI_FORMAT_R16G16B16A16_FLOAT, 8, true },
        { REX::W32::DXGI_FORMAT_R16G16B16A16_FLOAT, 8, false },
    };
    constexpr size_t Position_Candidate_Count = sizeof(Position_Candidates) / sizeof(Position_Candidates[0]);

    enum class PositionCalibrationState : uint8_t
    {
        e_measured,       // measured successfully, the calibration result is used
        e_desc_fallback,  // cannot calibrate (raw data missing, etc.) → fall back to desc derivation, do not skip
        e_unresolved,     // no candidate matched → skip this draw (better to draw too little than to smear garbage over the screen)
    };

    struct PositionCalibration
    {
        // Layout decision only (format/offset are desc attributes, so they can be cached per desc).
        REX::W32::DXGI_FORMAT format;
        uint32_t offset;
        PositionCalibrationState state;
    };

    // Decode a single position: the caller must already guarantee offset + bytes <= stride (bounds safety)
    bool decode_position(
        uint8_t const* base, uint32_t stride, uint32_t offset,
        uint32_t bytes, uint32_t index, RE::NiPoint3& out)
    {
        uint8_t const* src = base + static_cast<size_t>(index) * stride + offset;
        if (bytes == 12)
        {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            std::memcpy(&x, src + 0, sizeof(float));
            std::memcpy(&y, src + 4, sizeof(float));
            std::memcpy(&z, src + 8, sizeof(float));
            out = RE::NiPoint3{ x, y, z };
        }
        else
        {
            uint16_t half[4]{};
            std::memcpy(half, src, sizeof(half));
            out = RE::NiPoint3{
                DirectX::PackedVector::XMConvertHalfToFloat(half[0]),
                DirectX::PackedVector::XMConvertHalfToFloat(half[1]),
                DirectX::PackedVector::XMConvertHalfToFloat(half[2]),
            };
        }
        return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
    }

    // Sample-decode the model-space AABB centre/radius and min/max (the sample cap flattens large meshes); any non-finite value → failure
    bool measure_position(
        uint8_t const* base, uint32_t stride, uint32_t offset,
        uint32_t bytes, uint32_t vertex_count,
        RE::NiPoint3& center, float& radius, RE::NiPoint3& min, RE::NiPoint3& max)
    {
        uint32_t const step = (vertex_count > Calibration_Sample_Limit)
                                       ? (vertex_count + Calibration_Sample_Limit - 1) / Calibration_Sample_Limit
                                       : 1;
        RE::NiPoint3 min_p{};
        RE::NiPoint3 max_p{};
        bool first = true;
        for (uint32_t v = 0; v < vertex_count; v += step)
        {
            RE::NiPoint3 p{};
            if (!decode_position(base, stride, offset, bytes, v, p))
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

        center = RE::NiPoint3{ (min_p.x + max_p.x) * 0.5f, (min_p.y + max_p.y) * 0.5f, (min_p.z + max_p.z) * 0.5f };
        float const dx = max_p.x - min_p.x;
        float const dy = max_p.y - min_p.y;
        float const dz = max_p.z - min_p.z;
        radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
        min = min_p;
        max = max_p;
        return true;
    }

    // Decide and select the position layout; the first calibration of each desc emits one complete
    // evidence log line (INFO on success / WARN on no match); the degraded path logs INFO once. The
    // result is cached per desc (zero decoding in steady state).
    PositionCalibration calibrate_position_format(
        RE::BSGraphics::VertexDesc const& desc,
        RE::BSGraphics::TriShape const* renderer_data,
        uint32_t vertex_count,
        RE::NiBound const& model_bound,
        uint32_t stride)
    {
        uint64_t desc_raw = 0;
        std::memcpy(&desc_raw, &desc, sizeof(desc_raw));

        static std::vector<std::pair<uint64_t, PositionCalibration>> s_calibrations;
        for (auto const& [cached_raw, cached] : s_calibrations)
        {
            if (cached_raw == desc_raw)
                return cached;
        }

        uint32_t const desc_offset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_POSITION);
        bool const decodable = renderer_data && renderer_data->rawVertexData &&
                               desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) && stride > 0;

        PositionCalibration result{ .state = PositionCalibrationState::e_desc_fallback };
        std::string const desc_fields = fmt::format(
            "desc={:#018x} flags={:#06x} stride={} pos={} uv={} nrm={} bin={} col={} verts={} model_bound=({:.1f},{:.1f},{:.1f}) r={:.1f}",
            desc_raw,
            static_cast<unsigned>(desc.GetFlags()),
            stride,
            desc_offset,
            desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0),
            desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_NORMAL),
            desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_BINORMAL),
            desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_COLOR),
            vertex_count,
            model_bound.center.x, model_bound.center.y, model_bound.center.z, model_bound.radius);

        if (!decodable)
        {
            // Cannot calibrate → fall back to desc derivation (the current behaviour), log INFO once, do not skip
            result.format = position_format_of(desc);
            result.offset = desc_offset;
            result.state = PositionCalibrationState::e_desc_fallback;
            logger::info("{}",
                fmt::format("Mask overlay: position calibration unavailable (raw vertex data missing), using desc-derived layout {} format={:#06x} offset={}",
                    desc_fields, static_cast<unsigned>(result.format), result.offset));
            if (s_calibrations.size() < Max_Position_Calibrations)
                s_calibrations.emplace_back(desc_raw, result);
            return result;
        }

        struct CandidateResult
        {
            REX::W32::DXGI_FORMAT format;
            uint32_t offset;
            bool valid;          // survived candidate culling (stride/bounds)
            bool passed;         // matched modelBound
            RE::NiPoint3 center;
            float radius;
            float center_error;
        };
        CandidateResult results[Position_Candidate_Count]{};

        float const tolerance = 0.5f * std::max(model_bound.radius, 1.0f);
        for (size_t i = 0; i < Position_Candidate_Count; ++i)
        {
            PositionCandidate const& cand = Position_Candidates[i];
            CandidateResult& res = results[i];
            res.format = cand.format;
            res.offset = cand.from_desc ? desc_offset : 0;
            // Cull: the stride cannot hold the format, or offset+format runs past this vertex stride
            if (stride < cand.bytes || res.offset + cand.bytes > stride)
                continue;
            res.valid = true;

            RE::NiPoint3 center{};
            float radius = 0.0f;
            RE::NiPoint3 model_min{};
            RE::NiPoint3 model_max{};
            if (!measure_position(renderer_data->rawVertexData, stride, res.offset, cand.bytes, vertex_count, center, radius, model_min, model_max))
                continue;

            res.center = center;
            res.radius = radius;
            res.center_error = distance_to_point(center, model_bound.center);
            res.passed = res.center_error <= tolerance &&
                         radius >= 0.4f * model_bound.radius &&
                         radius <= 2.5f * model_bound.radius;
        }

        // Take the passing candidate with the smallest centre error; on a tie prefer float32 (conservative: read too many bytes rather than misread half precision)
        int best = -1;
        for (size_t i = 0; i < Position_Candidate_Count; ++i)
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
            bool const is_float32 = results[i].format == REX::W32::DXGI_FORMAT_R32G32B32_FLOAT;
            bool const best_is_float32 = results[best].format == REX::W32::DXGI_FORMAT_R32G32B32_FLOAT;
            if (error < best_error - 1e-4f || (std::fabs(error - best_error) <= 1e-4f && is_float32 && !best_is_float32))
                best = static_cast<int>(i);
        }

        std::string table;
        for (size_t i = 0; i < Position_Candidate_Count; ++i)
        {
            CandidateResult const& r = results[i];
            table += fmt::format(" [{} fmt={:#06x} off={} {} center=({:.1f},{:.1f},{:.1f}) r={:.1f} err={:.1f}]",
                i, static_cast<unsigned>(r.format), r.offset,
                r.passed ? "PASS" : (r.valid ? "fail" : "rejected"),
                r.center.x, r.center.y, r.center.z, r.radius, r.center_error);
        }

        if (best < 0)
        {
            result.state = PositionCalibrationState::e_unresolved;
            logger::warn("{}",
                fmt::format("Mask overlay: position calibration found no matching candidate, draw skipped {} candidates:{}",
                    desc_fields, table));
        }
        else
        {
            result.format = results[best].format;
            result.offset = results[best].offset;
            result.state = PositionCalibrationState::e_measured;
            logger::info("{}",
                fmt::format("Mask overlay: position calibration selected fmt={:#06x} off={} (center err {:.2f}) desc={:#018x}",
                    static_cast<unsigned>(result.format), result.offset, results[best].center_error, desc_raw));
        }

        if (s_calibrations.size() < Max_Position_Calibrations)
            s_calibrations.emplace_back(desc_raw, result);
        return result;
    }

    // ---------------------------------------------------------------------------
    // Skinned partition layout calibration: neither the stride (SKINNING byte count) nor the
    // position layout has an authoritative definition, so the static path's method is reused -
    // decode the partition's own rawVertexData and compare against the geometry modelBound. The
    // position format comes from the attribute offset spacing (for flags 0x5b/0x9/0x1b/0x3b the
    // attribute after the position always has offset 16, i.e. a float32 16-byte slot). Results are
    // cached per raw partition desc; no candidate passes or rawVertexData missing → e_unresolved
    // and the caller skips the draw.
    // ---------------------------------------------------------------------------

    enum class SkinnedCalibrationState : uint8_t
    {
        e_measured,    // measured successfully, the calibration result is used
        e_unresolved,  // no candidate passed / cannot calibrate → skip this draw
    };

    // Layout decision: the cache stores only these fields and no per-mesh validation verdict
    struct SkinnedVertexLayout
    {
        REX::W32::DXGI_FORMAT position_format;
        uint32_t position_offset;
        uint32_t stride;
        uint8_t layout_id;     // SKINNING layout id (1..4)
        MaskSkinLayout skin;
        SkinnedCalibrationState state;
    };

    // SKINNING layout candidates (the order is the priority; *_delta is the byte increment relative to the VA_SKINNING offset)
    struct SkinningLayoutSpec
    {
        uint8_t id;
        REX::W32::DXGI_FORMAT weight_format;
        uint32_t weight_bytes;
        uint32_t weight_delta;
        REX::W32::DXGI_FORMAT index_format;
        uint32_t index_delta;
    };

    // 1/2 are used when the available bytes A>=12; 3/4 are used when A==8
    constexpr SkinningLayoutSpec Skinning_Layouts[] = {
        { 1, REX::W32::DXGI_FORMAT_R16G16B16A16_FLOAT, 8, 0, REX::W32::DXGI_FORMAT_R8G8B8A8_UINT, 8 },
        { 2, REX::W32::DXGI_FORMAT_R16G16B16A16_FLOAT, 8, 4, REX::W32::DXGI_FORMAT_R8G8B8A8_UINT, 0 },
        { 3, REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM, 4, 0, REX::W32::DXGI_FORMAT_R8G8B8A8_UINT, 4 },
        { 4, REX::W32::DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, REX::W32::DXGI_FORMAT_R8G8B8A8_UINT, 0 },
    };
    constexpr size_t Skinning_Layout_Count = sizeof(Skinning_Layouts) / sizeof(Skinning_Layouts[0]);

    // Look up a candidate definition by layout id (the id comes from a cached layout decision so it always hits; return the first entry defensively)
    SkinningLayoutSpec const* find_skinning_layout(uint8_t id)
    {
        for (size_t i = 0; i < Skinning_Layout_Count; ++i)
        {
            if (Skinning_Layouts[i].id == id)
                return &Skinning_Layouts[i];
        }
        return &Skinning_Layouts[0];
    }

    // Per-mesh validation statistics: produced by a full vertex traversal over each mesh's own data;
    // indices are interpreted as global bone indices bounded by the palette length P
    struct SkinnedMeshStats
    {
        bool position_finite;                           // all vertices finite under the selected position format
        uint32_t index_min;
        uint32_t index_max;
        uint32_t out_of_range_index_count;         // number of vertices holding an index >= P slot
        uint32_t out_of_range_weighted_count;      // of those, the number of vertices whose (that slot's) weight is non-zero → reject
        uint32_t first_out_of_range_index;         // first out-of-range index
        float first_out_of_range_weight;                // its weight
        float weight_sum_min;
        float weight_sum_max;
        uint32_t bad_weight_vertices;              // number of vertices with an out-of-range weight component
    };

    // Per-mesh validation verdict: position failure → skip; weight failure → switch candidate;
    // out-of-range index: non-zero weight → caller skips, zero weight → drawn as usual
    enum class SkinnedMeshVerdict : uint8_t
    {
        e_ok,
        e_position_bad,  // positions non-finite → skip this mesh
        e_weights_bad,   // weight sum/components invalid → the cached layout does not fit this mesh, re-enumerate candidates
    };

    // Candidate measurement result (layout decision plus the validation statistics on this mesh, used for the log table)
    struct SkinnedLayoutCandidateResult
    {
        uint32_t stride;
        uint8_t layout;
        bool bounds_ok;          // bounds culling (no read performed)
        SkinnedMeshStats stats;
        bool passed;             // positions finite and weights valid (an out-of-range index does not affect passing)
    };

    // Decode the weight quadruple of a single vertex (R16G16B16A16_FLOAT / R8G8B8A8_UNORM; the caller already guarantees the read stays in bounds)
    void decode_weights(REX::W32::DXGI_FORMAT format, uint8_t const* src, float (&out)[4])
    {
        if (format == REX::W32::DXGI_FORMAT_R16G16B16A16_FLOAT)
        {
            uint16_t half[4]{};
            std::memcpy(half, src, sizeof(half));
            for (int i = 0; i < 4; ++i)
                out[i] = DirectX::PackedVector::XMConvertHalfToFloat(half[i]);
        }
        else
        {
            uint8_t byte[4]{};
            std::memcpy(byte, src, sizeof(byte));
            for (int i = 0; i < 4; ++i)
                out[i] = static_cast<float>(byte[i]) / 255.0f;
        }
    }

    // Decode the 4 bone indices of a single vertex (R8G8B8A8_UINT; the caller already guarantees the read stays in bounds)
    void decode_indices(uint8_t const* src, uint8_t (&out)[4])
    {
        std::memcpy(out, src, sizeof(out));
    }

    // Whether all weight components of a single vertex are within the allowed range
    bool weight_components_ok(float const (&w)[4])
    {
        for (int i = 0; i < 4; ++i)
        {
            if (w[i] < Skinned_Weight_Min || w[i] > Skinned_Weight_Max)
                return false;
        }
        return true;
    }

    // Traverse all vertices under the given position and SKINNING layout, fill in the statistics
    // and return the verdict. Indices are global bone indices with index_bound (the palette length
    // P) as the out-of-range bound; the caller guarantees every read stays within the stride.
    SkinnedMeshVerdict validate_skinned_mesh(
        uint8_t const* raw, uint32_t stride, uint32_t pos_offset, uint32_t pos_bytes,
        SkinningLayoutSpec const& spec, uint32_t weight_offset, uint32_t index_offset,
        uint32_t vertex_count, uint32_t index_bound, SkinnedMeshStats& stats)
    {
        stats = SkinnedMeshStats{ .position_finite = true };
        stats.index_min = 0xFFFFFFFFu;  // start from a sentinel and take the min over the indices (vertex count > 0 is guaranteed by the caller)
        bool first_sample = true;
        for (uint32_t v = 0; v < vertex_count; ++v)
        {
            RE::NiPoint3 p{};
            if (!decode_position(raw, stride, pos_offset, pos_bytes, v, p))
                stats.position_finite = false;

            uint8_t const* const base = raw + static_cast<size_t>(v) * stride;
            float w[4]{};
            decode_weights(spec.weight_format, base + weight_offset, w);
            uint8_t idx[4]{};
            decode_indices(base + index_offset, idx);

            float sum = 0.0f;
            for (int i = 0; i < 4; ++i)
                sum += w[i];
            if (first_sample)
            {
                stats.weight_sum_min = stats.weight_sum_max = sum;
                first_sample = false;
            }
            else
            {
                stats.weight_sum_min = std::min(stats.weight_sum_min, sum);
                stats.weight_sum_max = std::max(stats.weight_sum_max, sum);
            }
            if (!weight_components_ok(w))
                ++stats.bad_weight_vertices;

            bool vertex_out_of_range = false;
            bool vertex_weighted_out_of_range = false;
            for (int i = 0; i < 4; ++i)
            {
                uint32_t const index = idx[i];
                stats.index_min = std::min(stats.index_min, index);
                stats.index_max = std::max(stats.index_max, index);
                if (index >= index_bound)
                {
                    vertex_out_of_range = true;
                    if (w[i] != 0.0f)  // this out-of-range slot carries a non-zero weight → it cannot render correctly
                        vertex_weighted_out_of_range = true;
                }
            }
            if (vertex_out_of_range)
            {
                ++stats.out_of_range_index_count;
                if (stats.out_of_range_weighted_count == 0)
                {
                    // Record the first "out-of-range with non-zero weight" slot (as evidence for the skip log)
                    for (int i = 0; i < 4; ++i)
                    {
                        if (static_cast<uint32_t>(idx[i]) >= index_bound && w[i] != 0.0f)
                        {
                            stats.first_out_of_range_index = static_cast<uint32_t>(idx[i]);
                            stats.first_out_of_range_weight = w[i];
                            break;
                        }
                    }
                }
                if (vertex_weighted_out_of_range)
                    ++stats.out_of_range_weighted_count;
            }
        }

        if (!stats.position_finite)
            return SkinnedMeshVerdict::e_position_bad;
        bool const weights_ok = stats.bad_weight_vertices == 0 &&
                                stats.weight_sum_min >= Skinned_Weight_Sum_Min &&
                                stats.weight_sum_max <= Skinned_Weight_Sum_Max;
        return weights_ok ? SkinnedMeshVerdict::e_ok : SkinnedMeshVerdict::e_weights_bad;
    }

    // Candidate table text (built on the one-shot path; each entry holds stride/layout/position finiteness/weight sum/out-of-range counts/index)
    std::string format_skinned_candidate_table(
        SkinnedLayoutCandidateResult const (&candidates)[Max_Skinned_Layout_Candidates],
        MaskSkinLayout const (&skin)[Max_Skinned_Layout_Candidates],
        size_t count,
        uint32_t index_bound)
    {
        std::string table;
        for (size_t i = 0; i < count; ++i)
        {
            SkinnedLayoutCandidateResult const& c = candidates[i];
            SkinnedMeshStats const& s = c.stats;
            table += fmt::format(
                " [{} L{} stride={} w(fmt={:#06x},off={}) i(fmt={:#06x},off={}) {} pos_finite={} wsum=[{:.3f},{:.3f}] wbad={} imax={}/{} oob={}(w{})]",
                i, static_cast<unsigned>(c.layout), c.stride,
                static_cast<unsigned>(skin[i].weight_format), skin[i].weight_offset,
                static_cast<unsigned>(skin[i].index_format), skin[i].index_offset,
                c.passed ? "PASS" : (c.bounds_ok ? "fail" : "rejected"),
                s.position_finite, s.weight_sum_min, s.weight_sum_max, s.bad_weight_vertices,
                s.index_max, index_bound, s.out_of_range_index_count, s.out_of_range_weighted_count);
        }
        return table;
    }

    // Enumerate (stride, SKINNING layout) candidates and validate each on the mesh's own data;
    // the cache is neither read nor written.
    size_t enumerate_skinned_candidates(
        RE::BSGraphics::VertexDesc const& desc,
        RE::BSGraphics::TriShape const* renderer_data,
        uint32_t vertex_count,
        uint32_t index_bound,
        uint32_t pos_offset,
        uint32_t pos_bytes,
        uint32_t skin_offset,
        SkinnedLayoutCandidateResult (&candidates)[Max_Skinned_Layout_Candidates],
        MaskSkinLayout (&skin)[Max_Skinned_Layout_Candidates])
    {
        // Stride candidates (SKINNING 8/12 bytes, deduplicated)
        uint32_t const stride_options[2] = {
            vertex_size_of_with_skinning(desc, 8u),
            vertex_size_of_with_skinning(desc, 12u),
        };
        uint32_t strides[2]{};
        size_t stride_count = 0;
        for (uint32_t const stride : stride_options)
        {
            bool duplicate = false;
            for (size_t i = 0; i < stride_count; ++i)
                duplicate = duplicate || strides[i] == stride;
            if (!duplicate)
                strides[stride_count++] = stride;
        }

        bool const raw_available = renderer_data && renderer_data->rawVertexData && vertex_count > 0;
        uint8_t const* const raw = raw_available ? renderer_data->rawVertexData : nullptr;
        size_t candidate_count = 0;
        if (!raw_available)
            return 0;

        for (size_t si = 0; si < stride_count; ++si)
        {
            uint32_t const stride = strides[si];
            uint32_t const available = (stride > skin_offset) ? (stride - skin_offset) : 0u;
            for (size_t li = 0; li < Skinning_Layout_Count; ++li)
            {
                if (candidate_count >= Max_Skinned_Layout_Candidates)
                    break;  // defensive: stop once the candidate array is full (the current enumeration never exceeds its capacity)
                SkinningLayoutSpec const& spec = Skinning_Layouts[li];
                bool const eligible = (spec.id <= 2) ? (available >= 12u) : (available == 8u);
                if (!eligible)
                    continue;

                uint32_t const weight_offset = skin_offset + spec.weight_delta;
                uint32_t const index_offset = skin_offset + spec.index_delta;
                // Bounds safety: the position/weight/index reads must all stay within the stride, otherwise the candidate is unusable (nothing is read)
                bool const bounds_ok = (pos_offset + pos_bytes <= stride) &&
                                       (weight_offset + spec.weight_bytes <= stride) &&
                                       (index_offset + 4u <= stride);

                SkinnedLayoutCandidateResult& cr = candidates[candidate_count];
                cr.stride = stride;
                cr.layout = spec.id;
                cr.bounds_ok = bounds_ok;
                cr.stats.position_finite = true;
                cr.passed = false;
                skin[candidate_count] = MaskSkinLayout{ spec.weight_format, weight_offset, spec.index_format, index_offset };

                if (bounds_ok)
                {
                    SkinnedMeshVerdict const verdict = validate_skinned_mesh(
                        raw, stride, pos_offset, pos_bytes, spec, weight_offset, index_offset,
                        vertex_count, index_bound, cr.stats);
                    // An out-of-range index does not affect the candidate passing (the complete palette upload performed by the mask draw covers it)
                    cr.passed = verdict == SkinnedMeshVerdict::e_ok;
                }

                ++candidate_count;
            }
        }
        return candidate_count;
    }

    SkinnedVertexLayout calibrate_skinned_layout(
        RE::BSGraphics::VertexDesc const& desc,
        RE::BSGraphics::TriShape const* renderer_data,
        uint32_t vertex_count,
        uint32_t index_bound)
    {
        uint64_t desc_raw = 0;
        std::memcpy(&desc_raw, &desc, sizeof(desc_raw));

        static std::vector<std::pair<uint64_t, SkinnedVertexLayout>> s_skinned_calibrations;
        for (auto const& [cached_raw, cached] : s_skinned_calibrations)
        {
            if (cached_raw == desc_raw)
                return cached;
        }

        using V = RE::BSGraphics::Vertex;
        auto const attr_offset = [&desc](V::Attribute attr) {
            return desc.GetAttributeOffset(attr);
        };
        uint32_t const pos_offset = attr_offset(V::VA_POSITION);
        uint32_t const skin_offset = attr_offset(V::VA_SKINNING);

        // ---- The position format is determined by the attribute offset spacing (gap = offset of the
        // next later attribute - position offset). Evidence: in the static and skinned calibration
        // logs, for flags 0x5b/0x9/0x1b/0x3b the attribute immediately after the position always has
        // offset 16, i.e. the position occupies a 16-byte slot (float32). ----
        uint32_t next_offset = 0;
        bool has_next = false;
        auto const consider_next = [&](bool present, uint32_t offset) {
            if (!present || offset <= pos_offset)
                return;
            if (!has_next || offset < next_offset)
            {
                next_offset = offset;
                has_next = true;
            }
        };
        consider_next(desc.HasFlag(V::VF_UV), attr_offset(V::VA_TEXCOORD0));
        consider_next(desc.HasFlag(V::VF_UV_2), attr_offset(V::VA_TEXCOORD1));
        consider_next(desc.HasFlag(V::VF_NORMAL), attr_offset(V::VA_NORMAL));
        consider_next(desc.HasFlag(V::VF_TANGENT), attr_offset(V::VA_BINORMAL));
        consider_next(desc.HasFlag(V::VF_COLORS), attr_offset(V::VA_COLOR));
        consider_next(desc.HasFlag(V::VF_SKINNED), attr_offset(V::VA_SKINNING));
        consider_next(desc.HasFlag(V::VF_LANDDATA), attr_offset(V::VA_LANDDATA));
        consider_next(desc.HasFlag(V::VF_EYEDATA), attr_offset(V::VA_EYEDATA));

        int32_t const gap = has_next ? (static_cast<int32_t>(next_offset) - static_cast<int32_t>(pos_offset)) : -1;
        bool const gap_degenerate = gap < 8;

        SkinnedVertexLayout result{ .state = SkinnedCalibrationState::e_unresolved };
        result.position_offset = pos_offset;
        if (gap >= 12)
            result.position_format = REX::W32::DXGI_FORMAT_R32G32B32_FLOAT;
        else if (gap >= 8)
            result.position_format = REX::W32::DXGI_FORMAT_R16G16B16A16_FLOAT;
        else
            result.position_format = position_format_of(desc);  // degenerate: no later attribute / gap too small
        uint32_t const pos_bytes = (result.position_format == REX::W32::DXGI_FORMAT_R32G32B32_FLOAT) ? 12u : 8u;

        std::string const desc_fields = fmt::format(
            "desc={:#018x} flags={:#06x} pos={} uv={} nrm={} bin={} col={} skin={} gap={}{} pos_fmt={:#06x} pos_off={} verts={} index_bound={}",
            desc_raw,
            static_cast<unsigned>(desc.GetFlags()),
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
            vertex_count,
            index_bound);

        // ---- Candidate enumeration plus per-candidate validation on **this mesh's** data. Full
        // vertex traversal, no more sampling, and no validation verdict is read from or written to
        // the cache. ----
        SkinnedLayoutCandidateResult candidates[Max_Skinned_Layout_Candidates]{};
        MaskSkinLayout candidate_skin[Max_Skinned_Layout_Candidates]{};
        size_t const candidate_count = enumerate_skinned_candidates(
            desc, renderer_data, vertex_count, index_bound, pos_offset, pos_bytes, skin_offset,
            candidates, candidate_skin);

        if (candidate_count == 0)
        {
            result.state = SkinnedCalibrationState::e_unresolved;
            logger::warn("{}",
                fmt::format("Mask overlay: skinned layout calibration unavailable ({}), draw skipped {}",
                    (renderer_data && renderer_data->rawVertexData) ? "geometry has no usable candidate" : "raw vertex data missing",
                    desc_fields));
            if (s_skinned_calibrations.size() < Max_Skinned_Layout_Calibrations)
                s_skinned_calibrations.emplace_back(desc_raw, result);
            return result;
        }

        // Take the first candidate that passes everything (the priority is the enumeration order: stride order × layouts 1..4)
        int64_t best = -1;
        for (size_t i = 0; i < candidate_count; ++i)
        {
            if (candidates[i].passed)
            {
                best = static_cast<int64_t>(i);
                break;
            }
        }

        std::string const table = format_skinned_candidate_table(candidates, candidate_skin, candidate_count, index_bound);

        if (best < 0)
        {
            result.state = SkinnedCalibrationState::e_unresolved;
            logger::warn("{}",
                fmt::format("Mask overlay: skinned layout calibration found no matching candidate, draw skipped {} candidates:{}",
                    desc_fields, table));
        }
        else
        {
            result.stride = candidates[best].stride;
            result.layout_id = candidates[best].layout;
            result.skin = candidate_skin[best];
            result.state = SkinnedCalibrationState::e_measured;
            logger::info("{}",
                fmt::format("Mask overlay: skinned layout calibration selected pos(fmt={:#06x},off={}) stride={} layout={} weight(fmt={:#06x},off={}) index(fmt={:#06x},off={}) desc={:#018x}",
                    static_cast<unsigned>(result.position_format), result.position_offset,
                    result.stride, static_cast<unsigned>(result.layout_id),
                    static_cast<unsigned>(result.skin.weight_format), result.skin.weight_offset,
                    static_cast<unsigned>(result.skin.index_format), result.skin.index_offset, desc_raw));
        }

        // Only the layout decision is written to the cache (the result struct holds no per-mesh statistics)
        if (s_skinned_calibrations.size() < Max_Skinned_Layout_Calibrations)
            s_skinned_calibrations.emplace_back(desc_raw, result);
        return result;
    }

    // ---------------------------------------------------------------------------
    // Geometry collection
    // ---------------------------------------------------------------------------

    // Target context needed by the static draw checks: the position and the form id are taken once
    // per target in collect_draws and reused for every geometry.
    struct TargetContext
    {
        RE::NiPoint3 position;
        RE::FormID form_id;
        // The target 3D contains skinned geometry → its non-skinned geometry (icicles and similar decoration) is not drawn.
        bool has_skinned;
        // Integral target index; zero-based until the geometry shader upload.
        uint32_t target_index;
    };

    void collect_static(RE::BSGeometry* geom, RE::BSGeometry::GEOMETRY_RUNTIME_DATA const& geom_rt, TargetContext const& target, std::vector<MaskDraw>& draws)
    {
        // The target 3D contains skinned geometry → its static geometry (icicles and similar
        // decoration) is not part of the body silhouette and is not drawn.
        if (target.has_skinned)
        {
            char const* const node_name = geom->name.c_str();
            logger::info("{}",
                fmt::format("Mask overlay: skip static geometry on skinned corpse (static prop is not part of the body silhouette) target={:08X} node=\"{}\"",
                    target.form_id, node_name ? node_name : "?"));
            return;
        }

        // ---- Geometry-level GPU buffer check specific to the static path. The skinned path does not pass this gate. ----
        if (!geom_rt.rendererData || !geom_rt.rendererData->vertexBuffer || !geom_rt.rendererData->indexBuffer)
        {
            logger::debug("Mask overlay: skip geometry without GPU buffers");
            return;
        }

        // ---- Classification guard: these BSTriShape subclasses / data flags cannot be drawn as
        // ordinary static geometry - forcing a draw produces garbage triangles over the screen. ----
        char const* const rtti_name = geom->GetRTTI() ? geom->GetRTTI()->GetName() : "";
        char const* const node_name = geom->name.c_str();
        char const* exclusion_reason = nullptr;

        if (strcmp(rtti_name, "BSDynamicTriShape") == 0)
            exclusion_reason = "BSDynamicTriShape (dynamic vertex layout)";
        else if (strcmp(rtti_name, "BSInstanceTriShape") == 0)
            exclusion_reason = "BSInstanceTriShape (instanced, per-instance stream not bound)";
        else if (strcmp(rtti_name, "BSMultiStreamInstanceTriShape") == 0)
            exclusion_reason = "BSMultiStreamInstanceTriShape (multi-stream instanced, extra streams not bound)";
        else if (strcmp(rtti_name, "BSSubIndexTriShape") == 0)
            exclusion_reason = "BSSubIndexTriShape (decal sub-range index structure)";
        else if (geom_rt.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_INSTANCEDATA))
            exclusion_reason = "vertexDesc VF_INSTANCEDATA";
        else if (geom_rt.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_EYEDATA))
            exclusion_reason = "vertexDesc VF_EYEDATA";
        else if (geom_rt.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_LANDDATA))
            exclusion_reason = "vertexDesc VF_LANDDATA";

        if (exclusion_reason)
        {
            logger::info("{}",
                fmt::format("Mask overlay: skip static geometry ({}) rtti={} node=\"{}\"", exclusion_reason, rtti_name, node_name ? node_name : "?"));
            return;
        }

        RE::BSTriShape* tri = geom->AsTriShape();
        if (!tri)
        {
            // Regular geometry in a SSE scene is always of the BSTriShape family; other types are not masked
            logger::debug("Mask overlay: skip non-BSTriShape geometry");
            return;
        }

        auto const& tri_rt = tri->GetTrishapeRuntimeData();
        if (tri_rt.vertexCount == 0 || tri_rt.triangleCount == 0)
        {
            logger::info("{}",
                fmt::format("Mask overlay: skip empty geometry (verts={} tris={}) rtti={} node=\"{}\"",
                    tri_rt.vertexCount, tri_rt.triangleCount, rtti_name, node_name ? node_name : "?"));
            return;
        }

        RE::NiBound const& model_bound = geom->GetModelData().modelBound;
        if (model_bound.radius <= 0.0f)
        {
            logger::info("{}",
                fmt::format("Mask overlay: skip geometry with invalid model bound rtti={} node=\"{}\"", rtti_name, node_name ? node_name : "?"));
            return;
        }

        uint32_t const vertex_stride = vertex_size_of(geom_rt.vertexDesc);

        // ---- World bounding-sphere validation: modelBound extrapolated through the world transform,
        // the largest column norm of the 3×3 block as the anisotropic scale upper bound; a
        // non-finite/degenerate/over-budget sphere means effect-type/anomalous data. ----
        RE::NiTransform const& world_transform = geom->world;
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
        if (world_radius <= 0.0f || world_radius > Max_Part_World_Radius)
        {
            logger::info("{}",
                fmt::format("Mask overlay: skip static draw [world radius out of range] target={:08X} node=\"{}\" model_bound r={:.1f} world_radius={:.1f} cap={:.1f}",
                    target.form_id, node_name, model_bound.radius, world_radius, Max_Part_World_Radius));
            return;
        }

        // ---- Position format calibration: VF_FULLPREC is not assumed; rawVertexData is measured and
        // compared against modelBound to select the format+offset, cached per desc (zero decoding in
        // steady state); no candidate matches → skip this draw (better to draw too little than to
        // smear garbage over the screen); cannot calibrate → fall back to desc derivation. ----
        PositionCalibration const calibration = calibrate_position_format(
            geom_rt.vertexDesc, geom_rt.rendererData, tri_rt.vertexCount, model_bound, vertex_stride);
        if (calibration.state == PositionCalibrationState::e_unresolved)
            return;

        // ---- Ownership check: the mask only draws geometry "at the target" (engine world bounding
        // sphere, looser than a box test). The per-mesh model AABB only feeds calibration scoring
        // and never enters the cache or this test (to prevent cross-mesh pollution). ----
        float const ref_distance = distance_to_point(target.position, world_center);
        float const proximity_limit = world_radius + Ref_Proximity_Slack;
        if (ref_distance > proximity_limit)
        {
            logger::info("{}",
                fmt::format("Mask overlay: skip static draw [geometry not at target] target={:08X} node=\"{}\" distance={:.1f} limit={:.1f} ref=({:.1f},{:.1f},{:.1f}) world_center=({:.1f},{:.1f},{:.1f}) world_radius={:.1f}",
                    target.form_id, node_name,
                    ref_distance, proximity_limit,
                    target.position.x, target.position.y, target.position.z,
                    world_center.x, world_center.y, world_center.z, world_radius));
            return;
        }

        MaskDraw draw{};
        draw.vertex_buffer = geom_rt.rendererData->vertexBuffer;
        draw.index_buffer = geom_rt.rendererData->indexBuffer;
        draw.vertex_desc = geom_rt.vertexDesc;
        draw.node = geom;
        draw.node_ref.reset(geom);  // keep alive: if the geometry is unloaded, node/rendererData/VB/IB stay valid until the end of this frame
        draw.vertex_stride = vertex_stride;
        draw.vertex_count = tri_rt.vertexCount;
        draw.triangle_count = tri_rt.triangleCount;
        draw.index_count = static_cast<uint32_t>(tri_rt.triangleCount) * 3u;
        draw.position_format = calibration.format;
        draw.position_offset = calibration.offset;
        draw.target_index = target.target_index;
        draws.push_back(std::move(draw));
    }

    void collect_skinned(RE::BSGeometry* geom, RE::BSGeometry::GEOMETRY_RUNTIME_DATA const& geom_rt, TargetContext const& target, std::vector<MaskDraw>& draws)
    {
        char const* const node_name = geom->name.c_str();
        char const* const rtti_name = geom->GetRTTI() ? geom->GetRTTI()->GetName() : "?";

        RE::NiSkinInstance* skin = geom_rt.skinInstance.get();
        RE::NiSkinPartition* skin_partition = skin ? skin->skinPartition.get() : nullptr;
        if (!skin || !skin_partition || !skin->skinData || !skin->skinData->GetBoneData() || !skin->boneWorldTransforms || !skin->bones)
        {
            std::string missing;
            auto const note = [&missing](bool null, char const* field) {
                if (null)
                {
                    if (!missing.empty())
                        missing += ", ";
                    missing += field;
                }
            };
            note(!skin, "skinInstance");
            note(skin && !skin_partition, "skinPartition");
            note(skin && !skin->skinData, "skinData");
            note(skin && skin->skinData && !skin->skinData->GetBoneData(), "skinData->GetBoneData()");
            note(skin && !skin->boneWorldTransforms, "boneWorldTransforms");
            note(skin && !skin->bones, "bones");
            logger::warn("{}", fmt::format("Mask overlay: skip skinned draw [skin instance incomplete] node=\"{}\" {}", node_name,
                fmt::format("missing=[{}] rtti={}", missing, rtti_name ? rtti_name : "?")));
            return;
        }

        uint32_t const partition_count = std::min(skin_partition->numPartitions, static_cast<uint32_t>(skin_partition->partitions.size()));
        if (partition_count < skin_partition->numPartitions)
        {
            logger::info("{}", fmt::format("Mask overlay: skip skinned draw [partition count exceeds array size] node=\"{}\" {}", node_name,
                fmt::format("numPartitions={} partitions.size()={} extra partitions ignored",
                skin_partition->numPartitions, skin_partition->partitions.size())));
        }
        if (partition_count == 0)
        {
            logger::info("{}", fmt::format("Mask overlay: skip skinned draw [no skin partitions] node=\"{}\" {}", node_name,
                fmt::format("numPartitions={} partitions.size()={}", skin_partition->numPartitions, skin_partition->partitions.size())));
            return;
        }

        RE::NiAVObject* const root_parent = skin->rootParent;
        if (!root_parent)
        {
            logger::warn("{}", fmt::format("Mask overlay: skip skinned draw [skin instance has no rootParent] node=\"{}\" {}", node_name,
                fmt::format("rtti={} partitions={}", rtti_name ? rtti_name : "?", partition_count)));
            return;
        }

        // ---- modelBound is unusable for engine-managed skinned meshes (measured: every
        // body/equipment/hair mesh has r=0.0), so it must not gate the draw; the worldBound serves
        // as a sanity gate only when the engine provides one (radius > 0). ----
        RE::NiBound const& world_bound = geom->worldBound;
        if (world_bound.radius > 0.0f)
        {
            if (world_bound.radius > Max_Part_World_Radius)
            {
                logger::warn("{}", fmt::format("Mask overlay: skip skinned draw [world bound out of range] node=\"{}\" {}", node_name,
                    fmt::format("world_bound=({:.1f},{:.1f},{:.1f}) r={:.1f} cap={:.1f}",
                    world_bound.center.x, world_bound.center.y, world_bound.center.z, world_bound.radius,
                    Max_Part_World_Radius)));
                return;
            }
        }

        for (uint32_t p = 0; p < partition_count; ++p)
        {
            RE::NiSkinPartition::Partition const& part = skin_partition->partitions[p];
            RE::BSGraphics::TriShape* const buff = part.buffData;

            // The partition is rejected - name the exact condition plus the partition ordinal and vertices/triangles
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
                logger::info("{}", fmt::format("Mask overlay: skip skinned draw [{}] node=\"{}\" {}", reject, node_name,
                    fmt::format("partition={} vertices={} triangles={}", p, part.vertices, part.triangles)));
                continue;
            }

            if (part.strips != 0)
            {
                // A strip partition (stripLengths index layout) cannot be drawn as a triangle list - skip to avoid errors
                logger::info("{}", fmt::format("Mask overlay: skip skinned draw [partition is a triangle strip] node=\"{}\" {}", node_name,
                    fmt::format("partition={} strips={} vertices={} triangles={}", p, part.strips, part.vertices, part.triangles)));
                continue;
            }

            // ---- The palette is built in **global bone index space** (a vertex index is a subscript
            // into the skin bone array: measured numBones=10/index_max=61/skin_bones=61). Its valid
            // length is P = min(GetBoneCount(), numMatrices). part.bones/numBones feed diagnostic
            // logs only and take part in no decision. ----
            uint32_t const palette_count = palette_slot_count(skin);
            if (palette_count == 0 || palette_count > Max_Palette_Bones)
            {
                logger::warn("{}", fmt::format("Mask overlay: skip skinned draw [palette slot count out of range] node=\"{}\" {}", node_name,
                    fmt::format("partition={} P={} budget={}", p, palette_count, Max_Palette_Bones)));
                continue;
            }

            // ---- The position format is determined by the attribute offset spacing; the stride and the
            // SKINNING (weight/index) layout are self-calibrated from the mesh's own vertex data; with no
            // solution the draw is skipped (it must not fall back to an UNKNOWN position format or a
            // hard-coded skinning order). ----
            SkinnedVertexLayout calibration = calibrate_skinned_layout(
                buff->vertexDesc, buff, part.vertices, palette_count);
            if (calibration.state != SkinnedCalibrationState::e_measured)
                continue;

            // ---- Per-mesh validation runs on this mesh (no verdict reused from another mesh, full
            // vertex traversal); indices are global with P as the bound. Position failure → skip;
            // weights invalid → re-enumerate candidates (without writing the cache); out-of-range
            // index: non-zero weight → skip, zero weight → drawn (the replica fill makes it a
            // no-op). ----
            uint8_t const* const raw = buff->rawVertexData;
            uint32_t const pos_bytes = (calibration.position_format == REX::W32::DXGI_FORMAT_R32G32B32_FLOAT) ? 12u : 8u;
            SkinningLayoutSpec const* spec = find_skinning_layout(calibration.layout_id);

            SkinnedMeshStats stats{ .position_finite = true };
            SkinnedMeshVerdict verdict = validate_skinned_mesh(
                raw, calibration.stride, calibration.position_offset, pos_bytes, *spec,
                calibration.skin.weight_offset, calibration.skin.index_offset,
                part.vertices, palette_count, stats);

            if (verdict == SkinnedMeshVerdict::e_position_bad)
            {
                logger::warn("{}", fmt::format("Mask overlay: skip skinned draw [mesh positions non-finite] node=\"{}\" {}", node_name,
                    fmt::format("partition={} verts={} stride={} pos(fmt={:#06x},off={})",
                    p, part.vertices, calibration.stride, static_cast<unsigned>(calibration.position_format), calibration.position_offset)));
                continue;
            }
            if (verdict == SkinnedMeshVerdict::e_weights_bad)
            {
                // The cached layout does not fit this mesh: re-enumerate candidates on this mesh's data (without writing the cache)
                SkinnedLayoutCandidateResult candidates[Max_Skinned_Layout_Candidates]{};
                MaskSkinLayout candidate_skin[Max_Skinned_Layout_Candidates]{};
                uint32_t const skin_offset = buff->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING);
                size_t const candidate_count = enumerate_skinned_candidates(
                    buff->vertexDesc, buff, part.vertices, palette_count, calibration.position_offset, pos_bytes,
                    skin_offset, candidates, candidate_skin);
                int64_t switch_to = -1;
                for (size_t i = 0; i < candidate_count; ++i)
                {
                    if (candidates[i].passed)
                    {
                        switch_to = static_cast<int64_t>(i);
                        break;
                    }
                }
                if (switch_to < 0)
                {
                    logger::warn("{}", fmt::format("Mask overlay: skip skinned draw [no skinned layout fits this mesh] node=\"{}\" {}", node_name,
                        fmt::format("partition={} verts={} P={} cached_layout={} candidates:{}",
                        p, part.vertices, palette_count, static_cast<unsigned>(calibration.layout_id),
                        format_skinned_candidate_table(candidates, candidate_skin, candidate_count, palette_count))));
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
                if (verdict != SkinnedMeshVerdict::e_ok)
                {
                    logger::warn("{}", fmt::format("Mask overlay: skip skinned draw [switched layout still fails mesh validation] node=\"{}\" {}", node_name,
                        fmt::format("partition={} verts={} P={} stride={} layout={}",
                        p, part.vertices, palette_count, calibration.stride, static_cast<unsigned>(calibration.layout_id))));
                    continue;
                }
            }

            // index >= P **with a non-zero weight** → it cannot render correctly, skip + WARN (the
            // replica fill is a no-op for zero-weight slots, which are only counted in the stats).
            if (stats.out_of_range_weighted_count > 0)
            {
                logger::warn("{}", fmt::format("Mask overlay: skip skinned draw [bone index exceeds palette bounds with non-zero weight] node=\"{}\" {}", node_name,
                    fmt::format("partition={} P={} oob_weighted={} index_max={} first_oob_index={} first_oob_weight={:.4f}",
                    p, palette_count, stats.out_of_range_weighted_count, stats.index_max,
                    stats.first_out_of_range_index, stats.first_out_of_range_weight)));
                continue;
            }

            MaskDraw draw{};
            draw.skinned = true;
            draw.skin = geom_rt.skinInstance;
            draw.partition = p;
            draw.node = geom;
            draw.node_ref.reset(geom);  // keep the geometry alive
            draw.vertex_buffer = buff->vertexBuffer;
            draw.index_buffer = buff->indexBuffer;
            draw.vertex_desc = buff->vertexDesc;
            draw.vertex_stride = calibration.stride;
            draw.vertex_count = part.vertices;
            draw.triangle_count = part.triangles;
            draw.index_count = static_cast<uint32_t>(part.triangles) * 3u;
            draw.position_format = calibration.position_format;
            draw.position_offset = calibration.position_offset;
            draw.skin_layout = calibration.skin;
            draw.target_index = target.target_index;
            draws.push_back(std::move(draw));
        }
    }

    void collect_geometry(RE::BSGeometry* geom, TargetContext const& target, std::vector<MaskDraw>& draws)
    {
        switch (geom->GetType().get())
        {
        case RE::BSGeometry::Type::kParticles:
        case RE::BSGeometry::Type::kStripParticles:
        case RE::BSGeometry::Type::kParticleShaderDynamicTriShape:
        case RE::BSGeometry::Type::kLines:
        case RE::BSGeometry::Type::kDynamicLines:
        case RE::BSGeometry::Type::kInstanceGroup:
            logger::debug("Mask overlay: skip particle/line geometry (type {})", static_cast<int>(geom->GetType().get()));
            return;
        default:
            break;
        }

        RE::BSGeometry::GEOMETRY_RUNTIME_DATA const& geom_rt = geom->GetGeometryRuntimeData();

        if (geom_rt.shaderProperty && geom_rt.shaderProperty->GetRTTI() &&
            strcmp(geom_rt.shaderProperty->GetRTTI()->GetName(), "BSEffectShaderProperty") == 0)
        {
            logger::info("{}",
                fmt::format("Mask overlay: skip effect-shader geometry (BSEffectShaderProperty, fx attachment is not part of the corpse silhouette) node=\"{}\"",
                    geom->name.c_str() ? geom->name.c_str() : "?"));
            return;
        }

        if (geom_rt.skinInstance)
            collect_skinned(geom, geom_rt, target, draws);
        else
            collect_static(geom, geom_rt, target, draws);
    }
}

uint32_t palette_slot_count(RE::NiSkinInstance const* skin)
{
    if (!skin || !skin->skinData)
        return 0;
    uint32_t const skin_bones = skin->skinData->GetBoneCount();
    uint32_t const matrix_count = skin->numMatrices;
    return std::min(skin_bones, matrix_count);
}

void collect_mask_draws(std::vector<MaskTarget> const& targets, std::vector<MaskDraw>& draws)
{
    for (size_t ti = 0; ti < targets.size(); ++ti)
    {
        MaskTarget const& target = targets[ti];
        RE::TESObjectREFR* ref = target.ref.get();
        if (!ref)
            continue;

        RE::NiAVObject* root = ref->GetCurrent3D();
        if (!root)
            continue;

        bool has_skinned = false;
        RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* geom) {
            if (geom->GetGeometryRuntimeData().skinInstance)
            {
                has_skinned = true;
                return RE::BSVisit::BSVisitControl::kStop;
            }
            return RE::BSVisit::BSVisitControl::kContinue;
        });

        TargetContext const target_ctx{
            .position = ref->GetPosition(), .form_id = ref->GetFormID(), .has_skinned = has_skinned,
            .target_index = static_cast<uint32_t>(ti) };

        RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* geom) {
            if (draws.size() >= Max_Draws_Per_Frame)
            {
                logger::warn("Mask overlay: draw cap {} reached, extra geometry dropped", Max_Draws_Per_Frame);
                return RE::BSVisit::BSVisitControl::kStop;
            }
            collect_geometry(geom, target_ctx, draws);
            return RE::BSVisit::BSVisitControl::kContinue;
        });
    }
}

MASK_NAMESPACE_END
