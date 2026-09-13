#include "outline_mask.h"

#include "config/config.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>

PLUGIN_NAMESPACE_BEGIN

namespace
{
    // ---------------------------------------------------------------------------
    // mask RT（懒创建，渲染线程独占）：与后备缓冲同尺寸，RGBA8_UNORM，无深度缓冲。
    // 设备或尺寸变化时整体重建（同 renderer.cpp 的 ensure_back_buffer 模式）。
    // ---------------------------------------------------------------------------
    ID3D11Device* g_mask_device = nullptr;  // 仅用于设备变化比较，不持引用
    ID3D11Texture2D* g_mask_texture = nullptr;
    ID3D11RenderTargetView* g_mask_rtv = nullptr;
    ID3D11ShaderResourceView* g_mask_srv = nullptr;
    std::uint32_t g_mask_w = 0;
    std::uint32_t g_mask_h = 0;

    // ---------------------------------------------------------------------------
    // 自编译管线：static/skinned 两个 VS + mask/composite 两个 PS。
    // 蒙皮 VS 使用矩阵调色板常量缓冲（每分区一次 draw，palette 容量受限）。
    // ---------------------------------------------------------------------------
    constexpr std::size_t Max_Palette_Bones = 128;   // 调色板常量缓冲预算（矩阵/draw）；128×64B = 8KB
    constexpr std::size_t Palette_CB_Bytes = Max_Palette_Bones * 64;
    static_assert(Palette_CB_Bytes == Max_Palette_Bones * sizeof(float) * 16, "palette CB layout must be float4x4 slots");
    constexpr std::size_t Max_Draws_Per_Frame = 256;
    constexpr std::size_t Max_Corpse_Index = 255;  // 契约 v18：mask B 通道可精确存储的索引上限

    // ---- 静态 draw 足迹校验常量（契约 v4）：世界包围球 / 目标归属 ----
    constexpr float Max_Part_World_Radius = 1024.0f;  // 世界包围球半径上限（游戏单位）
    constexpr float Ref_Proximity_Slack = 256.0f;     // draw 足迹到目标 ref 位置的最小邻域（游戏单位）

    ID3D11VertexShader* g_vs_static = nullptr;
    ID3D11VertexShader* g_vs_skinned = nullptr;
    ID3D11VertexShader* g_vs_composite = nullptr;
    ID3D11PixelShader* g_ps_mask = nullptr;
    // ---- 诊断脚手架（定位“整屏品红/无剪影”问题；诊断完成后整体移除）----
    ID3D11PixelShader* g_ps_mask_skinned = nullptr;  // 诊断：蒙皮路径通道编码（G=1）
    // true：蒙皮几何按绑定姿态绘制（rootParent 世界矩阵，跳过骨骼调色板），
    // 用于区分“调色板数学错误”（bind 姿态剪影正确）与“步进/数量/投影错误”（仍整屏覆盖）。
    constexpr bool k_diag_bind_pose = false;  // 诊断完成：矩阵约定已修正，下轮用真实蒙皮验证调色板

    // ---- 休眠开关（契约 v3）：蒙皮路径暂缓——先验证静态剪影，下一修订置 true 重启。
    // 蒙皮代码（collect_skinned、调色板、蒙皮 VS、推导注释）全部保留并参与编译。----
    constexpr bool k_skinning_enabled = true;
    // ---- 诊断脚手架结束 ----
    ID3D11PixelShader* g_ps_composite = nullptr;
    ID3D11Buffer* g_composite_cb = nullptr;        // b0：float4（OutlineColor rgb + 填充系数）
    constexpr float Silhouette_Fill_Alpha = 0.5f;  // 剪影内部填充系数（契约 v14：维持既有亮度）

    // alpha LUT（契约 v18 R-03）：按目标序号存放 corpse_alpha，消费 PS 以 mask B 通道
    // 的尸体索引查表；256 个 alpha 以 64 个 float4 打包（1024 字节，CB 尺寸 16 对齐）。
    constexpr std::size_t Alpha_Lut_Floats = 256;
    constexpr std::size_t Alpha_Lut_CB_Bytes = Alpha_Lut_Floats * sizeof(float);
    static_assert(Alpha_Lut_CB_Bytes == 64 * sizeof(float) * 4);
    ID3D11Buffer* g_alpha_lut_cb = nullptr;
    ID3DBlob* g_vs_static_blob = nullptr;   // CreateInputLayout 需要 VS 字节码，随管线保留
    ID3DBlob* g_vs_skinned_blob = nullptr;
    ID3D11Buffer* g_per_draw_cb = nullptr;  // b0：row_major float4x4 + mask alpha（契约 v15，80 字节）
    ID3D11Buffer* g_palette_cb = nullptr;   // b1：row_major float4x4[Max_Palette_Bones]
    bool g_pipeline_ready = false;
    bool g_pipeline_failed = false;         // 创建失败后不再每帧重试（避免持续泄漏 D3D 对象）
    bool g_composite_ready = false;         // 合成（仅调试叠加）可用性：失败只禁用叠加，mask 渲染保持可用

    // ---- 描边 pass（契约 v12）：mask 的既定消费者。可选对象组与 composite 同模式：
    // 创建失败只禁描边（一次性 WARN），不影响 mask 渲染与叠加。----
    constexpr std::uint32_t Max_Outline_Radius = 6;   // PS 圆盘采样半径上限（(2r+1)² = 169 taps）
    constexpr float Outline_Alpha = 1.0f;             // 色带 alpha 恒 1.0（按距离淡出经 alpha LUT 生效）
    ID3D11PixelShader* g_ps_outline = nullptr;        // ps_5_0：mask 圆盘膨胀外描边
    ID3D11Buffer* g_outline_cb = nullptr;             // b0：texel/radius/pad + color（32 字节）
    bool g_outline_ready = false;                     // 描边管线可用性（失败只禁描边）

    // InputLayout 按 (蒙皮, 精度, 属性偏移, 步进) 缓存——属性偏移来自各 mesh 的
    // vertexDesc，逐 mesh 创建设备对象不可取，故缓存去重（尸体 mesh 布局种类极少）。
    struct LayoutKey
    {
        bool skinned;
        bool full_prec;
        std::uint32_t position_format;  // R-06：位置格式为标定结果，须入键防不同格式共用布局
        std::uint32_t position_offset;
        std::uint32_t skinning_offset;
        std::uint32_t stride;
        // 蒙皮权重/索引布局（契约 v9 R-05）：标定结果，须入键防不同布局共用同一
        // InputLayout（静态 draw 保持默认 0/UNKNOWN）。
        std::uint32_t weight_format;
        std::uint32_t weight_offset;
        std::uint32_t index_format;
        std::uint32_t index_offset;

        bool operator==(LayoutKey const&) const = default;
    };
    std::vector<std::pair<LayoutKey, ID3D11InputLayout*>> g_layout_cache;

    ID3D11BlendState* g_blend_mask_write = nullptr;    // MAX 混合：mask 取各 draw 覆盖的并集
    // （诊断脚手架：由“关闭混合”改为 MAX 混合，使静态/蒙皮两通道剪影在互相重叠时
    //  均保持可见；MAX 也是 mask 并集语义的正确永久形态，诊断后决定去留）
    ID3D11BlendState* g_blend_premul_alpha = nullptr;  // 预乘 alpha（同 renderer.cpp 混合约定）
    ID3D11DepthStencilState* g_depth_disabled = nullptr;  // 深度测试关闭 —— mask 天然穿墙
    ID3D11RasterizerState* g_raster_cull_none = nullptr;
    ID3D11SamplerState* g_sampler_mask = nullptr;

    constexpr float Mask_Clear_Color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    // 目标列表（契约 v15/v18 R-02）：NiPointer 保活 + 互斥锁 + 距离衰减不透明度
    //（corpse_alpha(distance)，按目标序号填入消费 pass 的 per-frame alpha LUT）。
    // 线程模型见 outline_mask.h。
    std::mutex g_target_mutex;
    struct MaskTarget
    {
        RE::NiPointer<RE::TESObjectREFR> ref;
        float opacity;
    };
    std::vector<MaskTarget> g_targets;

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
    // 刻意不用 BSGraphics::State 的 viewProj——AE 实测有坏值（见 renderer.cpp 注释）。
    // ---------------------------------------------------------------------------
    struct Mat4
    {
        float m[4][4];

        static constexpr Mat4 identity()
        {
            return Mat4{ { { 1.0f, 0.0f, 0.0f, 0.0f },
                           { 0.0f, 1.0f, 0.0f, 0.0f },
                           { 0.0f, 0.0f, 1.0f, 0.0f },
                           { 0.0f, 0.0f, 0.0f, 1.0f } } };
        }

        // NiTransform → 4x4：原样消费 entry[row][col]（不转置）。
        // 实证：world-variant 中原样消费算得的世界包围球心与引擎 worldBound.center
        // 完全一致（转置消费偏差明显）。
        static Mat4 from_transform(RE::NiTransform const& a_transform)
        {
            Mat4 r = identity();
            for (int row = 0; row < 3; ++row)
            {
                for (int col = 0; col < 3; ++col)
                    r.m[row][col] = a_transform.rotate.entry[row][col] * a_transform.scale;
                r.m[row][3] = a_transform.translate[row];
            }
            return r;
        }

        // 引擎 worldToCam → Mat4：原样行主序拷贝（不转置）。
        // 五点解算实证（1e-7 一致性）：成员 WorldPtToScreenPt3 等价于
        // clip = W2C_raw·p（列向量消费，clip.w = 第 3 行·p），视锥斜率已烘焙在
        // 行 0/1 内（非单位范数 1.19/2.12），无需视锥矩阵与端口映射。
        static Mat4 from_world_to_cam_raw(float const (&a_src)[4][4])
        {
            Mat4 r{};
            for (int row = 0; row < 4; ++row)
                for (int col = 0; col < 4; ++col)
                    r.m[row][col] = a_src[row][col];
            return r;
        }

        // 上传字节取向自动校准用（地面真值直传预期成立；失败时转置字节序）
        Mat4 transposed() const
        {
            Mat4 r{};
            for (int row = 0; row < 4; ++row)
                for (int col = 0; col < 4; ++col)
                    r.m[row][col] = m[col][row];
            return r;
        }

        Mat4 operator*(Mat4 const& a_rhs) const  // (A*B)(v) = A(B(v))
        {
            Mat4 r{};
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

    // 常量缓冲上传字节取向（地面真值自动校准；直传预期成立，失败时转置字节序）
    bool g_mvp_upload_transposed = false;

    Mat4 oriented(Mat4 const& a_m)
    {
        return g_mvp_upload_transposed ? a_m.transposed() : a_m;
    }

    // ---------------------------------------------------------------------------
    // 顶点布局：由 vertexDesc 推导步进与属性格式。
    // CLibNG 的 VertexDesc::GetSize() 对半精度位置固定按 16 字节计（仅全精度成立）、
    // UV 固定按 4 字节计（全精度为 8），与真实步进不符；改为“各属性 offset+size
    // 取最大值”——offset 直接来自引擎写入的 desc（权威布局），属性大小为 SSE
    // 固定格式：
    //   POSITION   FULLPREC ? R32G32B32(12) : R16G16B16A16(8)
    //   TEXCOORDn  FULLPREC ? R32G32(8)     : R16G16(4)
    //   NORMAL/TANGENT/COLOR/EYEDATA/LANDDATA 4 字节
    //   SKINNING   权重 4×f16(8) + 索引 4×u8(4)，权重在前
    // ---------------------------------------------------------------------------
    // SKINNING 块字节数可参数化（契约 v8）：蒙皮分区步进未知，需按 8/12 两值试探；
    // 静态路径经下面的 vertex_size_of 固定传 12，行为与既有逐位一致。
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

    // Mat4（世界变换）变换 3D 点：列向量消费，含平移（约定见 Mat4 注释）
    RE::NiPoint3 transform_point(Mat4 const& a_m, RE::NiPoint3 const& a_p)
    {
        return RE::NiPoint3{
            a_m.m[0][0] * a_p.x + a_m.m[0][1] * a_p.y + a_m.m[0][2] * a_p.z + a_m.m[0][3],
            a_m.m[1][0] * a_p.x + a_m.m[1][1] * a_p.y + a_m.m[1][2] * a_p.z + a_m.m[1][3],
            a_m.m[2][0] * a_p.x + a_m.m[2][1] * a_p.y + a_m.m[2][2] * a_p.z + a_m.m[2][3],
        };
    }

    float distance_to_point(RE::NiPoint3 const& a_a, RE::NiPoint3 const& a_b)
    {
        float const dx = a_a.x - a_b.x;
        float const dy = a_a.y - a_b.y;
        float const dz = a_a.z - a_b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // ---------------------------------------------------------------------------
    // 位置属性格式标定（契约 v6）：desc 的 VF_FULLPREC 不可靠（0x1b/0x3b 未置位，
    // 但位置实为 float32 全精度 16 字节槽位——按 half4 声明会让 D3D11 把每个
    // float32 的前 8 字节当 4 个 half 读，位置全乱、三角形被拉到整屏）。故不再
    // 假定格式，而是用 rawVertexData 实测解码，与引擎 modelBound（模型空间地面
    // 真值）比对，选出吻合的 (格式, 字节偏移)，按 desc 缓存。
    // ---------------------------------------------------------------------------
    constexpr std::size_t Max_Position_Calibrations = 256;   // 标定缓存上限（超出退化不缓存）
    constexpr std::uint32_t Calibration_Sample_Limit = 256;  // 大网格采样上限

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
        // 逐 mesh 的模型 AABB 只服务候选评分，不进缓存（契约 v7：防跨网格污染）。
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
            // R-05：无法标定 → 退回 desc 推导（当前行为），一次性 INFO，不跳过
            result.format = position_format_of(a_desc);
            result.offset = desc_offset;
            result.state = PositionCalibrationState::kDescFallback;
            logger::info("outline mask: position calibration unavailable (raw vertex data missing), using desc-derived layout {} format={:#06x} offset={}",
                desc_fields, static_cast<unsigned>(result.format), result.offset);
            if (s_calibrations.size() < Max_Position_Calibrations)
                s_calibrations.emplace_back(desc_raw, result);
            return result;
        }

        struct CandidateResult
        {
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            std::uint32_t offset = 0;
            bool valid = false;  // 通过候选剔除（stride/越界）
            bool passed = false; // 与 modelBound 吻合
            RE::NiPoint3 center{};
            float radius = 0.0f;
            float center_error = 0.0f;
            RE::NiPoint3 model_min{};
            RE::NiPoint3 model_max{};
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
            res.model_min = model_min;
            res.model_max = model_max;
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
            logger::warn("outline mask: position calibration found no matching candidate, draw skipped {} candidates:{}",
                desc_fields, table);
        }
        else
        {
            result.format = results[best].format;
            result.offset = results[best].offset;
            result.state = PositionCalibrationState::kMeasured;
            logger::info("outline mask: position calibration selected fmt={:#06x} off={} (center err {:.2f}) {} candidates:{}",
                static_cast<unsigned>(result.format), result.offset, results[best].center_error, desc_fields, table);
        }

        if (s_calibrations.size() < Max_Position_Calibrations)
            s_calibrations.emplace_back(desc_raw, result);
        return result;
    }

    // ---------------------------------------------------------------------------
    // 蒙皮分区顶点布局标定（契约 v8）：蒙皮分区步进（SKINNING 块字节数）与位置布局
    // 均无权威定义可核，故与静态路径同法——用分区自带 rawVertexData 实测解码，与
    // 几何 modelBound（模型空间地面真值，覆盖网格全部顶点，分区是其子集）比对：
    // 全部采样点有限且落在 1.10×球内即通过。候选为 位置格式 × 位置偏移 × 步进 的
    // 三维组合。结果按分区 desc 原始值独立缓存（与静态 s_calibrations 分离，判据
    // 不同）；无候选通过或 rawVertexData 缺失 → kUnresolved，调用方跳过并告警。
    // ---------------------------------------------------------------------------
    constexpr std::size_t Max_Skinned_Layout_Calibrations = 256;  // 蒙皮标定缓存上限（超出退化不缓存）
    constexpr std::size_t Max_Skinned_Layout_Candidates = 4;      // 每步进最多 2 个 SKINNING 布局 × 2 步进
    // R-04 权重校验收紧阈值（SSE 顶点权重和约定为 1，阈值已刻意放宽）
    constexpr float Skinned_Weight_Min = -0.001f;
    constexpr float Skinned_Weight_Max = 1.001f;
    constexpr float Skinned_Weight_Sum_Min = 0.98f;
    constexpr float Skinned_Weight_Sum_Max = 1.02f;

    enum class SkinnedCalibrationState
    {
        kMeasured,    // 实测通过，使用标定结果
        kUnresolved,  // 无候选通过/无法标定 → 跳过该 draw
    };

    // 蒙皮顶点缓冲内的权重/索引布局（R-05：接入 InputLayout）
    struct MaskSkinLayout
    {
        DXGI_FORMAT weight_format = DXGI_FORMAT_UNKNOWN;
        std::uint32_t weight_offset = 0;
        DXGI_FORMAT index_format = DXGI_FORMAT_UNKNOWN;
        std::uint32_t index_offset = 0;
    };

    // 布局决策（契约 v10 R-01：缓存**只**保存这些字段，不含任何逐网格校验结论）
    struct SkinnedVertexLayout
    {
        DXGI_FORMAT position_format = DXGI_FORMAT_UNKNOWN;  // R-03：属性偏移间距判定
        std::uint32_t position_offset = 0;
        std::uint32_t stride = 0;                           // R-04：候选步进
        std::uint8_t layout_id = 0;                         // SKINNING 布局编号（1..4）
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

    // ①/② 用于可用字节 A>=12；③/④ 用于 A==8（见契约 R-04）
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

    // R-01（契约 v11）：调色板长度 P = min(skinData 骨骼数, skin 世界矩阵数)。顶点骨骼
    // 索引是 skin 骨骼数组的**全局下标**，故调色板按全局下标空间构建，P 为其有效长度。
    // 唯一权威实现，供 collect_skinned（校验上界/守卫）与 draw_mask_pass（构建/守卫）共用。
    std::uint32_t palette_slot_count(RE::NiSkinInstance const* a_skin)
    {
        if (!a_skin || !a_skin->skinData)
            return 0;
        std::uint32_t const skin_bones = a_skin->skinData->GetBoneCount();
        std::uint32_t const matrix_count = a_skin->numMatrices;
        return std::min(skin_bones, matrix_count);
    }

    // 逐网格校验统计（契约 v11 R-03：对每个网格在自身数据上全顶点遍历得出；索引按
    // **全局骨骼下标**解释，上界为调色板长度 P）
    struct SkinnedMeshStats
    {
        bool position_finite = true;             // 选定位置格式下全部顶点有限
        std::uint32_t index_min = 0;
        std::uint32_t index_max = 0;
        std::uint32_t out_of_range_index_count = 0;   // 含 index >= P 槽位的顶点数
        std::uint32_t out_of_range_weighted_count = 0;  // 其中（该槽位）权重非零的顶点数 → 拒绝
        std::uint32_t first_out_of_range_index = 0;   // 首个越界索引
        float first_out_of_range_weight = 0.0f;       // 其权重
        float weight_sum_min = 0.0f;
        float weight_sum_max = 0.0f;
        std::uint32_t bad_weight_vertices = 0;   // 存在权重分量越界的顶点数
    };

    // 逐网格校验结论（R-03：位置失败 → 跳过；权重失败 → 换候选；索引越界且非零权重由
    // 调用方据 stats 判定为跳过，索引越界但零权重照常绘制）
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
        bool passed = false;     // 位置有限 且 权重合格（索引越界不影响通过，由 R-03 兜底）
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

    // ---------------------------------------------------------------------------
    // 布局决策（缓存）与逐网格校验（契约 v10 R-01/R-02）。
    // 缓存**只**保存布局决策（按 desc 原始 8 字节值），不保存任何逐网格结论；每个
    // 蒙皮网格的校验一律在其自身数据上执行（全顶点遍历），绝不复用他网格的结论。
    // ---------------------------------------------------------------------------

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
                    // 索引越界不影响候选通过（由 draw_mask_pass 的完整调色板上传兜底）
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

        // ---- R-03：位置格式由属性偏移间距判定（gap = 下一个更靠后属性的偏移 - 位置偏移）。
        // 证据：本机 static 标定与蒙皮标定日志中 flags 0x5b/0x9/0x1b/0x3b 的位置后紧邻
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

        // ---- R-04（契约 v10 R-02）：候选枚举 + 逐候选在**本网格**数据上校验。
        // 全顶点遍历、不再抽样，且不读写缓存中的任何校验结论。----
        SkinnedLayoutCandidateResult candidates[Max_Skinned_Layout_Candidates];
        MaskSkinLayout candidate_skin[Max_Skinned_Layout_Candidates];
        std::size_t const candidate_count = enumerate_skinned_candidates(
            a_desc, a_renderer_data, a_vertex_count, a_index_bound, pos_offset, pos_bytes, skin_offset,
            candidates, candidate_skin);

        if (candidate_count == 0)
        {
            result.state = SkinnedCalibrationState::kUnresolved;
            logger::warn(
                "outline mask: skinned layout calibration unavailable ({}), draw skipped {}",
                (a_renderer_data && a_renderer_data->rawVertexData) ? "geometry has no usable candidate" : "raw vertex data missing",
                desc_fields);
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
            logger::warn("outline mask: skinned layout calibration found no matching candidate, draw skipped {} candidates:{}",
                desc_fields, table);
        }
        else
        {
            // position_format/position_offset 已由 R-03 填好
            result.stride = candidates[best].stride;
            result.layout_id = candidates[best].layout;
            result.skin = candidate_skin[best];
            result.state = SkinnedCalibrationState::kMeasured;
            logger::info(
                "outline mask: skinned layout calibration selected pos(fmt={:#06x},off={}) stride={} layout={} weight(fmt={:#06x},off={}) index(fmt={:#06x},off={}) {} candidates:{}",
                static_cast<unsigned>(result.position_format), result.position_offset,
                result.stride, static_cast<unsigned>(result.layout_id),
                static_cast<unsigned>(result.skin.weight_format), result.skin.weight_offset,
                static_cast<unsigned>(result.skin.index_format), result.skin.index_offset,
                desc_fields, table);
        }

        // R-01：缓存**只**写入布局决策（result 结构内不含任何逐网格统计）
        if (s_skinned_calibrations.size() < Max_Skinned_Layout_Calibrations)
            s_skinned_calibrations.emplace_back(desc_raw, result);
        return result;
    }

    // ---------------------------------------------------------------------------
    // 几何收集（渲染线程独占的临时列表；VB/IB 为游戏对象借用）。生存期说明：
    // 静态路径借用的 node/rendererData/VB/IB 由 node_ref（几何体 NiPointer）保活，
    // 防止游戏线程在本帧收集与绘制之间卸载目标 3D 导致悬垂；蒙皮路径的
    // buffData/VB/IB 由 skin（NiSkinInstance→skinPartition→buffData）引用链保活。
    // ---------------------------------------------------------------------------
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
        // desc 推导）；蒙皮路径写入 R-03 的属性偏移间距判定结果（绝不为 UNKNOWN）。
        DXGI_FORMAT position_format = DXGI_FORMAT_UNKNOWN;
        std::uint32_t position_offset = 0;
        bool skinned = false;                    // true：按分区 draw，调色板蒙皮
        RE::NiPointer<RE::NiSkinInstance> skin;  // 保活蒙皮实例（骨骼世界矩阵）
        std::uint32_t partition = 0;
        // R-02（契约 v18）：尸体索引（目标序号/255），经 per-draw CB → VS → mask PS
        // 的 B 通道输出（MAX 混合取重叠尸体的较高索引），消费 pass 据此查 alpha LUT。
        float corpse_index = 0.0f;
        // 蒙皮权重/索引布局（契约 v9 R-05）：仅蒙皮 draw 写入标定结果，供 get_layout
        // 生成 BLENDWEIGHT/BLENDINDICES；静态 draw 保持默认值且 get_layout 不读取。
        MaskSkinLayout skin_layout{};
    };

    // 静态 draw 校验（R-02..R-05）所需的目标上下文：位置与 form id 在 collect_draws
    // 处按目标取一次，逐 geometry 复用。
    struct TargetContext
    {
        RE::NiPoint3 position{};
        RE::FormID form_id = 0;
        // R-04（契约 v11）：目标 3D 中含蒙皮几何 → 其非蒙皮几何（冰锥等装饰）不画。
        bool has_skinned = false;
        // R-02（契约 v18）：尸体索引（目标序号/255），mask PS 写入 B 通道供 LUT 查表。
        float corpse_index = 0.0f;
    };

    // R-05：静态 draw 跳过定位日志——按 (目标 form id, 节点名, 原因) 签名一次性
    // INFO（附关键数值），同签名不重复输出。签名容器只被渲染线程访问（Present
    // 回调内），仅首次命中各签名时增长，不与目标列表锁交互。
    void log_static_skip_once(RE::FormID a_form_id, char const* a_node_name, std::string_view a_reason, std::string_view a_details)
    {
        static std::unordered_set<std::string> s_signatures;
        char const* const node = a_node_name ? a_node_name : "?";
        if (s_signatures.emplace(fmt::format("{:08X}|{}|{}", a_form_id, node, a_reason)).second)
            logger::info("outline mask: skip static draw [{}] target={:08X} node=\"{}\" {}", a_reason, a_form_id, node, a_details);
    }

    // R-07：蒙皮路径放弃/失败出口的定位日志——按 (节点名, 原因) 签名一次性输出
    // （附数值），同签名不重复。签名容器只被渲染线程访问（Present 回调内），仅首次
    // 命中各签名时增长，不与目标列表锁交互。
    void log_skinned_skip_once(bool a_warn, char const* a_node_name, std::string_view a_reason, std::string_view a_details)
    {
        static std::unordered_set<std::string> s_signatures;
        char const* const node = a_node_name ? a_node_name : "?";
        if (!s_signatures.emplace(fmt::format("{}|{}", node, a_reason)).second)
            return;
        if (a_warn)
            logger::warn("outline mask: skip skinned draw [{}] node=\"{}\" {}", a_reason, node, a_details);
        else
            logger::info("outline mask: skip skinned draw [{}] node=\"{}\" {}", a_reason, node, a_details);
    }

    // 蒙皮路径非跳过类一次性 INFO（如包围球报告）——按 (节点名, 原因) 签名去重。
    void log_skinned_info_once(char const* a_node_name, std::string_view a_reason, std::string_view a_details)
    {
        static std::unordered_set<std::string> s_signatures;
        char const* const node = a_node_name ? a_node_name : "?";
        if (s_signatures.emplace(fmt::format("{}|{}", node, a_reason)).second)
            logger::info("outline mask: skinned draw [{}] node=\"{}\" {}", a_reason, node, a_details);
    }

    // R-07：collect_skinned 结束汇总（本几何新增蒙皮 draw 数）——按节点名一次性。
    void log_skinned_summary_once(char const* a_node_name, std::string_view a_details)
    {
        static std::unordered_set<std::string> s_signatures;
        char const* const node = a_node_name ? a_node_name : "?";
        if (s_signatures.emplace(std::string{ node }).second)
            logger::info("outline mask: skinned collect summary node=\"{}\" {}", node, a_details);
    }

    void collect_static(RE::BSGeometry* a_geom, RE::BSGeometry::GEOMETRY_RUNTIME_DATA const& a_geom_rt, TargetContext const& a_target, std::vector<MaskDraw>& a_draws)
    {
        // ---- R-04（契约 v11）：目标 3D 含蒙皮几何时，其非蒙皮几何（冰锥等装饰）不属于
        // 尸体本体，一律不画——剪影只反映蒙皮身体。纯静态目标（灰烬堆/静态尸体容器等）
        // has_skinned 为假，行为不变。----
        if (a_target.has_skinned)
        {
            char const* const node_name = a_geom->name.c_str();
            static std::unordered_set<std::string> s_signatures;
            if (s_signatures.emplace(fmt::format("{}|{:08X}", node_name ? node_name : "?", a_target.form_id)).second)
            {
                logger::info("outline mask: skip static geometry on skinned corpse (static prop is not part of the body silhouette) target={:08X} node=\"{}\"",
                    a_target.form_id, node_name ? node_name : "?");
            }
            return;
        }

        // ---- R-01（契约 v8）：静态路径专属的几何级 GPU 缓冲检查（自 collect_geometry
        // 原样移入，语义与一次性 debug 日志不变）。蒙皮路径不走此门。----
        if (!a_geom_rt.rendererData || !a_geom_rt.rendererData->vertexBuffer || !a_geom_rt.rendererData->indexBuffer)
        {
            logger::debug("outline mask: skip geometry without GPU buffers");
            return;
        }

        // ---- 分类守卫（诊断迭代 2）：这些 BSTriShape 子类不能按普通静态几何绘制
        // （动态顶点布局 / 实例化第二顶点流 / 子范围索引结构），vertexDesc 数据标志
        // 同理——强行绘制会产生覆盖大半屏幕的垃圾三角形。每类原因一次性 INFO 记录。
        char const* const rtti_name = a_geom->GetRTTI() ? a_geom->GetRTTI()->GetName() : "";
        char const* const node_name = a_geom->name.c_str();
        bool excluded = false;
        char const* reason = "";

        if (strcmp(rtti_name, "BSDynamicTriShape") == 0)
        {
            static bool s_reported = false;
            excluded = true;
            reason = "BSDynamicTriShape (dynamic vertex layout)";
            if (!s_reported)
            {
                s_reported = true;
                logger::info("outline mask: skip static geometry ({}) rtti={} node=\"{}\"", reason, rtti_name, node_name ? node_name : "?");
            }
        }
        else if (strcmp(rtti_name, "BSInstanceTriShape") == 0)
        {
            static bool s_reported = false;
            excluded = true;
            reason = "BSInstanceTriShape (instanced, per-instance stream not bound)";
            if (!s_reported)
            {
                s_reported = true;
                logger::info("outline mask: skip static geometry ({}) rtti={} node=\"{}\"", reason, rtti_name, node_name ? node_name : "?");
            }
        }
        else if (strcmp(rtti_name, "BSMultiStreamInstanceTriShape") == 0)
        {
            static bool s_reported = false;
            excluded = true;
            reason = "BSMultiStreamInstanceTriShape (multi-stream instanced, extra streams not bound)";
            if (!s_reported)
            {
                s_reported = true;
                logger::info("outline mask: skip static geometry ({}) rtti={} node=\"{}\"", reason, rtti_name, node_name ? node_name : "?");
            }
        }
        else if (strcmp(rtti_name, "BSSubIndexTriShape") == 0)
        {
            static bool s_reported = false;
            excluded = true;
            reason = "BSSubIndexTriShape (decal sub-range index structure)";
            if (!s_reported)
            {
                s_reported = true;
                logger::info("outline mask: skip static geometry ({}) rtti={} node=\"{}\"", reason, rtti_name, node_name ? node_name : "?");
            }
        }
        else if (a_geom_rt.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_INSTANCEDATA))
        {
            static bool s_reported = false;
            excluded = true;
            reason = "vertexDesc VF_INSTANCEDATA";
            if (!s_reported)
            {
                s_reported = true;
                logger::info("outline mask: skip static geometry ({}) rtti={} node=\"{}\"", reason, rtti_name, node_name ? node_name : "?");
            }
        }
        else if (a_geom_rt.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_EYEDATA))
        {
            static bool s_reported = false;
            excluded = true;
            reason = "vertexDesc VF_EYEDATA";
            if (!s_reported)
            {
                s_reported = true;
                logger::info("outline mask: skip static geometry ({}) rtti={} node=\"{}\"", reason, rtti_name, node_name ? node_name : "?");
            }
        }
        else if (a_geom_rt.vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_LANDDATA))
        {
            static bool s_reported = false;
            excluded = true;
            reason = "vertexDesc VF_LANDDATA";
            if (!s_reported)
            {
                s_reported = true;
                logger::info("outline mask: skip static geometry ({}) rtti={} node=\"{}\"", reason, rtti_name, node_name ? node_name : "?");
            }
        }

        if (excluded)
            return;

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
            static bool s_empty_reported = false;
            if (!s_empty_reported)
            {
                s_empty_reported = true;
                logger::info("outline mask: skip empty geometry (verts={} tris={}) rtti={} node=\"{}\"",
                    tri_rt.vertexCount, tri_rt.triangleCount, rtti_name, node_name ? node_name : "?");
            }
            return;
        }

        RE::NiBound const& model_bound = a_geom->GetModelData().modelBound;
        if (model_bound.radius <= 0.0f)
        {
            static bool s_bound_reported = false;
            if (!s_bound_reported)
            {
                s_bound_reported = true;
                logger::info("outline mask: skip geometry with invalid model bound rtti={} node=\"{}\"", rtti_name, node_name ? node_name : "?");
            }
            return;
        }

        std::uint32_t const vertex_stride = vertex_size_of(a_geom_rt.vertexDesc);

        // ---- R-02（契约 v4）：世界包围球校验。modelBound 经节点世界变换外推，
        // 3×3 各列范数最大值作为各向异性缩放上界；非有限/退化/超预算的世界球
        // 说明该网格不适合按普通静态几何画进 mask（效果类/异常数据）。----
        Mat4 const world = Mat4::from_transform(a_geom->world);
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

        // ---- 位置格式标定（契约 v6）：不再假定 VF_FULLPREC，按 rawVertexData 实测与
        // modelBound 比对，选出格式+偏移并按 desc 缓存（稳态零解码）；无候选吻合
        // → 跳过该 draw（宁可少画不许垃圾涂屏）；无法标定 → 退回 desc 推导。----
        PositionCalibration const calibration = calibrate_position_format(
            a_geom_rt.vertexDesc, a_geom_rt.rendererData, tri_rt.vertexCount, model_bound, vertex_stride);
        if (calibration.state == PositionCalibrationState::kUnresolved)
            return;

        // ---- R-04（契约 v7）：归属校验——mask 只画“在目标处”的几何。统一使用 R-02
        // 已算出的引擎元数据世界包围球（比盒判定更宽松；v4/v5 实测零误跳）。
        // 逐 mesh 模型 AABB 只服务标定的候选评分，不进缓存、不参与归属判定（防跨
        // 网格污染：同一 desc 的不同网格必须用自身数据判定）。----
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
        draw.position_format = calibration.format;   // R-06：InputLayout 用标定结果
        draw.position_offset = calibration.offset;
        draw.corpse_index = a_target.corpse_index;  // R-02（契约 v18）：尸体索引
        a_draws.push_back(std::move(draw));
    }

    void collect_skinned(RE::BSGeometry* a_geom, RE::BSGeometry::GEOMETRY_RUNTIME_DATA const& a_geom_rt, TargetContext const& a_target, std::vector<MaskDraw>& a_draws)
    {
        char const* const node_name = a_geom->name.c_str();
        char const* const rtti_name = a_geom->GetRTTI() ? a_geom->GetRTTI()->GetName() : "?";

        // ---- R-07：skinInstance 分支被真正进入的首次记录（本轮关键验证信号）----
        static bool s_branch_reached_reported = false;
        if (!s_branch_reached_reported)
        {
            s_branch_reached_reported = true;
            logger::info("outline mask: skinInstance branch reached (skinning path active) rtti={} node=\"{}\"",
                rtti_name ? rtti_name : "?", node_name ? node_name : "?");
        }

        std::size_t const draws_before = a_draws.size();

        RE::NiSkinInstance* skin = a_geom_rt.skinInstance.get();
        RE::NiSkinPartition* skin_partition = skin ? skin->skinPartition.get() : nullptr;
        if (!skin || !skin_partition || !skin->skinData || !skin->skinData->GetBoneData() || !skin->boneWorldTransforms || !skin->bones)
        {
            // R-07：指名哪些字段为空
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
            // R-07：numPartitions == 0（含分区数组为空）——原实现完全静默
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

        // ---- R-01（契约 v9）：弃用 modelBound 前提。引擎管理的蒙皮网格 modelBound
        // 实为 0（11:43 实测：全部身体/装备/毛发网格 r=0.0），不得据此跳过。只做
        // 一次性 INFO 记录；包围球仅在引擎确实提供（worldBound.radius > 0）时用作
        // 廉价 sanity gate——非有限或超预算才跳过（一次性 WARN）。----
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

            // R-07：分区被拒——指名具体条件 + 分区序号 + vertices/triangles（原实现静默）
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

            // ---- R-01（契约 v11）：调色板按**全局骨骼索引空间**构建（顶点索引即 skin
            // 骨骼数组下标：实测 numBones=10/index_max=61/skin_bones=61）。有效长度
            // P = min(GetBoneCount(), numMatrices)。part.bones/numBones 此后仅供诊断日志，
            // 不再参与任何判定（原基于 part.bones 的调色板与 numBones 预算守卫已删除）。----
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
                // R-01：P 守卫取代原 numBones 预算守卫
                log_skinned_skip_once(true, node_name, "palette slot count out of range",
                    fmt::format("partition={} P={} skin_bones={} numMatrices={} budget={}",
                        p, palette_count, skin_bone_count, matrix_count, Max_Palette_Bones));
                continue;
            }

            // ---- R-03/R-04：位置格式按属性偏移间距判定；步进与 SKINNING（权重/索引）
            // 布局按网格自身顶点数据自标定；无解则跳过（不得退回 UNKNOWN 位置格式或
            // 硬编码蒙皮顺序——那是 v8 不可见的原因）。----
            SkinnedVertexLayout calibration = calibrate_skinned_layout(
                buff->vertexDesc, buff, part.vertices, palette_count);
            if (calibration.state != SkinnedCalibrationState::kMeasured)
                continue;

            // ---- R-02/R-04（契约 v10/v11）：逐网格校验**对本网格**执行（不复用他网格
            // 结论，全顶点遍历）；索引按全局下标、上界为 P。位置失败 → 跳过；权重不合 →
            // 重新枚举候选（不写缓存）；索引越界且带非零权重 → 跳过；越界但该槽位权重为
            // 0 → 照常绘制（副本填充使其为无操作），仅在诊断中计数。----
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

            // R-05：每个蒙皮网格首次一条诊断 INFO（含 P/skin_bones/numMatrices 与越界计数）
            log_skinned_info_once(node_name, "mesh diagnostics",
                fmt::format("rtti={} verts={} numBones={} P={} skin_bones={} numMatrices={} index=[{},{}] oob={} oob_weighted={} wsum=[{:.3f},{:.3f}] wbad={} pos(fmt={:#06x},off={}) stride={} layout={}",
                    rtti_name ? rtti_name : "?", part.vertices, part.numBones, palette_count,
                    skin_bone_count, matrix_count,
                    stats.index_min, stats.index_max, stats.out_of_range_index_count, stats.out_of_range_weighted_count,
                    stats.weight_sum_min, stats.weight_sum_max, stats.bad_weight_vertices,
                    static_cast<unsigned>(calibration.position_format), calibration.position_offset,
                    calibration.stride, static_cast<unsigned>(calibration.layout_id)));

            // R-03：索引 >= P 且**带非零权重** → 无法正确渲染，跳过 + WARN（R-03 的整块
            // 副本填充只对零权重槽位是无操作，故零权重越界不在此列，仅在诊断中计数）。
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
            draw.node = a_geom;           // 诊断记录转储用（生存期见 node_ref）
            draw.node_ref.reset(a_geom);  // 保活几何体（记录转储与后续检查需要）
            draw.vertex_buffer = reinterpret_cast<ID3D11Buffer*>(buff->vertexBuffer);
            draw.index_buffer = reinterpret_cast<ID3D11Buffer*>(buff->indexBuffer);
            draw.vertex_desc = buff->vertexDesc;
            draw.vertex_stride = calibration.stride;  // R-04：标定步进
            draw.vertex_count = part.vertices;
            draw.triangle_count = part.triangles;
            draw.index_count = static_cast<std::uint32_t>(part.triangles) * 3u;
            draw.position_format = calibration.position_format;  // R-03：绝不为 UNKNOWN
            draw.position_offset = calibration.position_offset;
            draw.skin_layout = calibration.skin;  // R-05：权重/索引格式与偏移
            draw.corpse_index = a_target.corpse_index;   // R-02（契约 v18）：尸体索引
            a_draws.push_back(std::move(draw));
        }

        // R-07：本几何新增蒙皮分区 draw 数汇总
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

        // ---- R-01（契约 v4）：效果着色器几何（BSEffectShaderProperty：发光
        // sprite/拖尾/粒子类 fx 附件）不是实体表面，顶点数据也不保证按“模型空间
        // 实体表面”语义解释——整类排除。首次命中 INFO（含首个节点名），此后静默。----
        if (geom_rt.shaderProperty && geom_rt.shaderProperty->GetRTTI() &&
            strcmp(geom_rt.shaderProperty->GetRTTI()->GetName(), "BSEffectShaderProperty") == 0)
        {
            static bool s_effect_shader_reported = false;
            if (!s_effect_shader_reported)
            {
                s_effect_shader_reported = true;
                char const* const node_name = a_geom->name.c_str();
                logger::info("outline mask: skip effect-shader geometry (BSEffectShaderProperty, fx attachment is not part of the corpse silhouette) node=\"{}\"",
                    node_name ? node_name : "?");
            }
            return;
        }

        // ---- R-01（契约 v8）：几何级 GPU 缓冲检查是静态路径专属——蒙皮路径不需要
        // geom_rt.rendererData，它用 NiSkinPartition::Partition::buffData（分区自带
        // VB/IB）。该检查已移入 collect_static，使下面的 skinInstance 分支先被求值；
        // 放在此处会静默丢弃全部蒙皮几何（此前蒙皮网格从未到达蒙皮分支的根因）。----

        // 蒙皮判定：skinInstance 非空 → 蒙皮路径（契约 v3 推迟）：k_skinning_enabled
        // 为 false 时跳过并一次性 INFO 记录；置 true 即整体重启蒙皮路径。
        if (geom_rt.skinInstance)
        {
            if (k_skinning_enabled)
                collect_skinned(a_geom, geom_rt, a_target, a_draws);
            else
            {
                static bool s_skinned_deferred_reported = false;
                if (!s_skinned_deferred_reported)
                {
                    s_skinned_deferred_reported = true;
                    char const* const rtti_name = a_geom->GetRTTI() ? a_geom->GetRTTI()->GetName() : "?";
                    char const* const node_name = a_geom->name.c_str();
                    logger::info(
                        "outline mask: skinned geometry skipped (skinning path deferred to next revision) rtti={} node=\"{}\"",
                        rtti_name ? rtti_name : "?",
                        node_name ? node_name : "?");
                }
            }
            return;
        }
        collect_static(a_geom, geom_rt, a_target, a_draws);
    }

    void collect_draws(std::vector<MaskTarget> const& a_targets, std::vector<MaskDraw>& a_draws)
    {
        // R-02（契约 v18）：目标列表顺序即尸体索引（renderer.cpp 构造顺序 = corpses
        // 快照顺序）；超出索引槽位的目标 clamp 到最后一个索引并一次性 WARN。
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

            // ---- R-04（契约 v11）：先做一次**短路**蒙皮探测（遇到任何蒙皮几何即停），
            // 供 collect_static 判定是否剔除该目标的静态装饰。----
            bool has_skinned = false;
            RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geom) {
                if (a_geom->GetGeometryRuntimeData().skinInstance)
                {
                    has_skinned = true;
                    return RE::BSVisit::BSVisitControl::kStop;
                }
                return RE::BSVisit::BSVisitControl::kContinue;
            });

            // 静态 draw 校验（R-02..R-05）所需的目标上下文：每目标取一次
            //（corpse_index = 目标序号/255，契约 v18 R-02 随上下文传入收集与绘制）
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

    // ---------------------------------------------------------------------------
    // 着色器：mask VS/PS 与调试叠加的合成 VS/PS（运行时编译，同 renderer.cpp 模式）
    // ---------------------------------------------------------------------------
    ID3DBlob* compile(char const* a_source, char const* a_target, char const* a_name)
    {
        if (!a_source || !a_target)
            return nullptr;

        ID3DBlob* blob = nullptr;
        ID3DBlob* err = nullptr;
        HRESULT const hr = D3DCompile(a_source, std::strlen(a_source), nullptr, nullptr, nullptr, "main", a_target, 0, 0, &blob, &err);
        if (FAILED(hr))
        {
            logger::error(
                "outline mask shader compile failed [{} {}] ({:X}): {}",
                a_name ? a_name : "?",
                a_target,
                static_cast<unsigned int>(hr),
                err ? static_cast<char const*>(err->GetBufferPointer()) : "no diagnostics");
            if (blob)
            {
                blob->Release();
                blob = nullptr;
            }
        }
        if (err)
            err->Release();

        return blob;
    }

    // 静态 VS：世界变换已在 CPU 侧组合进 ViewProj*World；尸体索引经
    // nointerpolation 语义传给 mask PS 写入 B 通道（契约 v18 R-02，同一 draw 恒定）
    char const* Vs_Static_Source = R"(
        cbuffer PerDrawCB : register(b0)
        {
            row_major float4x4 g_world_view_proj;
            float g_corpse_index;
            float3 g_pad;
        };

        struct VS_IN
        {
            float3 pos : POSITION;
        };

        struct VS_OUT
        {
            float4 pos : SV_Position;
            nointerpolation float corpse_index : TEXCOORD0;
        };

        VS_OUT main(VS_IN a_in)
        {
            VS_OUT o;
            o.pos = mul(g_world_view_proj, float4(a_in.pos, 1.0f));
            o.corpse_index = g_corpse_index;
            return o;
        }
    )";

    // 蒙皮 VS：VB 自带混合索引/权重，索引指向分区调色板 g_bones；尸体索引传递同静态 VS
    char const* Vs_Skinned_Source = R"(
        cbuffer PerDrawCB : register(b0)
        {
            row_major float4x4 g_world_view_proj;
            float g_corpse_index;
            float3 g_pad;
        };
        cbuffer PaletteCB : register(b1)
        {
            row_major float4x4 g_bones[128];
        };

        struct VS_IN
        {
            float3 pos : POSITION;
            float4 weights : BLENDWEIGHT;
            uint4 indices : BLENDINDICES;
        };

        struct VS_OUT
        {
            float4 pos : SV_Position;
            nointerpolation float corpse_index : TEXCOORD0;
        };

        VS_OUT main(VS_IN a_in)
        {
            float4 p = 0.0f;
            [unroll]
            for (int i = 0; i < 4; ++i)
                p += a_in.weights[i] * mul(g_bones[a_in.indices[i]], float4(a_in.pos, 1.0f));
            VS_OUT o;
            o.pos = mul(g_world_view_proj, p);
            o.corpse_index = g_corpse_index;
            return o;
        }
    )";

    char const* Vs_Composite_Source = R"(
        struct VS_OUT
        {
            float4 pos : SV_Position;
            float2 uv : TEXCOORD0;
        };

        VS_OUT main(uint a_id : SV_VertexID)
        {
            VS_OUT o;
            float2 uv = float2(float((a_id << 1u) & 2u), float(a_id & 2u));
            o.uv = uv;
            o.pos = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
            return o;
        }
    )";

    // ---- 诊断脚手架：通道编码 mask —— R=静态路径覆盖，G=蒙皮路径覆盖（诊断后还原为单色白）；
    // B = 尸体索引（契约 v18 R-02，MAX 混合取重叠尸体较高索引），A = 1 ----
    char const* Ps_Mask_Source = R"(
        struct PS_IN
        {
            float4 pos : SV_Position;
            nointerpolation float corpse_index : TEXCOORD0;
        };

        float4 main(PS_IN a_in) : SV_Target
        {
            return float4(1.0f, 0.0f, a_in.corpse_index, 1.0f);  // 诊断：静态路径 → R
        }
    )";

    char const* Ps_Mask_Skinned_Source = R"(
        struct PS_IN
        {
            float4 pos : SV_Position;
            nointerpolation float corpse_index : TEXCOORD0;
        };

        float4 main(PS_IN a_in) : SV_Target
        {
            return float4(0.0f, 1.0f, a_in.corpse_index, 1.0f);  // 诊断：蒙皮路径 → G
        }
    )";
    // ---- 诊断脚手架结束 ----

    // 合成 PS（契约 v14 R-01）：silhouette 模式的内部填充，静态/蒙皮统一为单色
    // OutlineColor（覆盖度取 max(R,G)——mask pass 的通道编码保留，不影响视觉）。
    // alpha = coverage × Silhouette_Fill_Alpha（0.5 维持既有亮度）。OM 为预乘 alpha
    // 混合（ONE/INV_SRC_ALPHA，同 renderer.cpp），必须输出预乘颜色。
    char const* Ps_Composite_Source = R"(
        Texture2D g_mask : register(t0);
        SamplerState g_mask_sampler : register(s0);

        cbuffer CompositeCB : register(b0)
        {
            float4 g_color;  // rgb + 填充系数
        };
        cbuffer AlphaLutCB : register(b1)
        {
            float4 g_alpha_lut[64];  // 256 个 corpse_alpha（按目标序号）
        };

        struct PS_IN
        {
            float4 pos : SV_Position;
            float2 uv : TEXCOORD0;
        };

        float4 main(PS_IN a_in) : SV_Target
        {
            // 注意：HLSL/FXC 不接受 "float const"（east const 仅适用于 C++ 代码，不适用于着色器串）
            const float4 m = g_mask.Sample(g_mask_sampler, a_in.uv);
            // R-04（契约 v18）：B 通道 = 尸体索引，查 per-frame alpha LUT（单次消费无复合）
            const uint idx = min(255u, (uint)round(m.b * 255.0f));
            const float a = max(m.r, m.g) * g_alpha_lut[idx / 4][idx % 4] * g_color.a;
            return float4(g_color.rgb * a, a);
        }
    )";

    // 描边 PS（契约 v12 R-02）：对 mask 做圆盘膨胀，仅在剪影**外侧**画 OutlineColor
    // 色带（中心覆盖度 > 0.5 即输出透明，内部不填充——内部显示由既有验证叠加负责）。
    // 覆盖度 = max(R, G)，与诊断通道编码兼容（脚手架拆除后该公式依然正确）。
    // 预乘 alpha 混合（同 composite/renderer.cpp 约定），故输出 rgb*a。
    char const* Ps_Outline_Source = R"(
        Texture2D g_mask : register(t0);
        SamplerState g_mask_sampler : register(s0);

        cbuffer OutlineCB : register(b0)
        {
            float2 g_texel;   // (1/W, 1/H)
            float g_radius;   // 圆盘半径（像素）
            float g_pad;
            float4 g_color;   // rgb + a
        };
        cbuffer AlphaLutCB : register(b1)
        {
            float4 g_alpha_lut[64];  // 256 个 corpse_alpha（按目标序号）
        };

        struct PS_IN
        {
            float4 pos : SV_Position;
            float2 uv : TEXCOORD0;
        };

        float coverage(float2 a_uv)
        {
            // 注意：HLSL/FXC 不接受 "float const"（east const 仅适用于 C++ 代码，不适用于着色器串）
            const float4 m = g_mask.Sample(g_mask_sampler, a_uv);
            return max(m.r, m.g);
        }

        float4 main(PS_IN a_in) : SV_Target
        {
            if (coverage(a_in.uv) > 0.5f)
                return float4(0.0f, 0.0f, 0.0f, 0.0f);  // 剪影内部：不画

            // R-04（契约 v18）：命中样本按 B 通道索引查 alpha LUT，取最大值
            const int r = (int)g_radius;
            float hit_alpha = 0.0f;
            [loop]
            for (int dy = -r; dy <= r; ++dy)
            {
                [loop]
                for (int dx = -r; dx <= r; ++dx)
                {
                    const float4 m = g_mask.Sample(g_mask_sampler, a_in.uv + float2(float(dx), float(dy)) * g_texel);
                    if (max(m.r, m.g) > 0.5f)
                    {
                        const uint idx = min(255u, (uint)round(m.b * 255.0f));
                        hit_alpha = max(hit_alpha, g_alpha_lut[idx / 4][idx % 4]);
                    }
                }
            }
            const float final_a = hit_alpha * g_color.a;
            return float4(g_color.rgb * final_a, final_a);
        }
    )";

    void release_mask_target()
    {
        if (g_mask_srv)
        {
            g_mask_srv->Release();
            g_mask_srv = nullptr;
        }
        if (g_mask_rtv)
        {
            g_mask_rtv->Release();
            g_mask_rtv = nullptr;
        }
        if (g_mask_texture)
        {
            g_mask_texture->Release();
            g_mask_texture = nullptr;
        }
        for (auto& entry : g_layout_cache)
        {
            if (entry.second)
                entry.second->Release();
        }
        g_layout_cache.clear();
        g_mask_w = 0;
        g_mask_h = 0;
        g_mask_device = nullptr;
    }

    void release_pipeline()
    {
        if (g_palette_cb)
        {
            g_palette_cb->Release();
            g_palette_cb = nullptr;
        }
        if (g_per_draw_cb)
        {
            g_per_draw_cb->Release();
            g_per_draw_cb = nullptr;
        }
        if (g_sampler_mask)
        {
            g_sampler_mask->Release();
            g_sampler_mask = nullptr;
        }
        if (g_raster_cull_none)
        {
            g_raster_cull_none->Release();
            g_raster_cull_none = nullptr;
        }
        if (g_depth_disabled)
        {
            g_depth_disabled->Release();
            g_depth_disabled = nullptr;
        }
        if (g_blend_premul_alpha)
        {
            g_blend_premul_alpha->Release();
            g_blend_premul_alpha = nullptr;
        }
        if (g_blend_mask_write)
        {
            g_blend_mask_write->Release();
            g_blend_mask_write = nullptr;
        }
        if (g_vs_static_blob)
        {
            g_vs_static_blob->Release();
            g_vs_static_blob = nullptr;
        }
        if (g_vs_skinned_blob)
        {
            g_vs_skinned_blob->Release();
            g_vs_skinned_blob = nullptr;
        }
        if (g_ps_composite)
        {
            g_ps_composite->Release();
            g_ps_composite = nullptr;
        }
        if (g_composite_cb)  // 契约 v14：composite 常量缓冲
        {
            g_composite_cb->Release();
            g_composite_cb = nullptr;
        }
        if (g_alpha_lut_cb)  // 契约 v18：alpha LUT 常量缓冲
        {
            g_alpha_lut_cb->Release();
            g_alpha_lut_cb = nullptr;
        }
        if (g_ps_outline)  // 描边 pass（契约 v12）
        {
            g_ps_outline->Release();
            g_ps_outline = nullptr;
        }
        if (g_outline_cb)
        {
            g_outline_cb->Release();
            g_outline_cb = nullptr;
        }
        if (g_ps_mask)
        {
            g_ps_mask->Release();
            g_ps_mask = nullptr;
        }
        if (g_ps_mask_skinned)  // 诊断脚手架
        {
            g_ps_mask_skinned->Release();
            g_ps_mask_skinned = nullptr;
        }
        if (g_vs_composite)
        {
            g_vs_composite->Release();
            g_vs_composite = nullptr;
        }
        if (g_vs_skinned)
        {
            g_vs_skinned->Release();
            g_vs_skinned = nullptr;
        }
        if (g_vs_static)
        {
            g_vs_static->Release();
            g_vs_static = nullptr;
        }
        g_pipeline_ready = false;
        g_composite_ready = false;
        g_outline_ready = false;
    }

    bool ensure_mask_target(ID3D11Device* a_device, std::uint32_t a_width, std::uint32_t a_height)
    {
        if (g_mask_device == a_device && g_mask_w == a_width && g_mask_h == a_height && g_mask_srv)
            return true;

        // 设备变化时，依附于旧设备的全部管线对象一并重建
        if (g_mask_device && g_mask_device != a_device)
            release_pipeline();
        release_mask_target();

        D3D11_TEXTURE2D_DESC td{};
        td.Width = a_width;
        td.Height = a_height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        HRESULT const tex_hr = a_device->CreateTexture2D(&td, nullptr, &g_mask_texture);
        if (FAILED(tex_hr) || !g_mask_texture)
        {
            logger::error("outline mask: failed to create mask texture ({:X})", static_cast<unsigned int>(tex_hr));
            release_mask_target();
            return false;
        }
        HRESULT const rtv_hr = a_device->CreateRenderTargetView(g_mask_texture, nullptr, &g_mask_rtv);
        HRESULT const srv_hr = a_device->CreateShaderResourceView(g_mask_texture, nullptr, &g_mask_srv);
        if (FAILED(rtv_hr) || !g_mask_rtv || FAILED(srv_hr) || !g_mask_srv)
        {
            logger::error("outline mask: failed to create mask views (rtv={:X}, srv={:X})", static_cast<unsigned int>(rtv_hr), static_cast<unsigned int>(srv_hr));
            release_mask_target();
            return false;
        }

        g_mask_device = a_device;
        g_mask_w = a_width;
        g_mask_h = a_height;
        return true;
    }

    bool ensure_mask_pipeline(ID3D11Device* a_device)
    {
        if (g_pipeline_ready || g_pipeline_failed)
            return g_pipeline_ready;

        // ---- mask 关键对象：任一失败则整体禁用 mask 渲染 ----
        g_vs_static_blob = compile(Vs_Static_Source, "vs_5_0", "outline mask static");
        g_vs_skinned_blob = compile(Vs_Skinned_Source, "vs_5_0", "outline mask skinned");
        ID3DBlob* ps_mask_blob = compile(Ps_Mask_Source, "ps_5_0", "outline mask static");  // 诊断：静态通道
        ID3DBlob* ps_mask_skinned_blob = compile(Ps_Mask_Skinned_Source, "ps_5_0", "outline mask skinned");  // 诊断：蒙皮通道
        if (!g_vs_static_blob || !g_vs_skinned_blob || !ps_mask_blob || !ps_mask_skinned_blob)
        {
            if (ps_mask_blob)
                ps_mask_blob->Release();
            if (ps_mask_skinned_blob)
                ps_mask_skinned_blob->Release();
            release_pipeline();
            g_pipeline_failed = true;
            logger::error("outline mask pipeline creation failed, mask rendering disabled");
            return false;
        }

        a_device->CreateVertexShader(g_vs_static_blob->GetBufferPointer(), g_vs_static_blob->GetBufferSize(), nullptr, &g_vs_static);
        a_device->CreateVertexShader(g_vs_skinned_blob->GetBufferPointer(), g_vs_skinned_blob->GetBufferSize(), nullptr, &g_vs_skinned);
        a_device->CreatePixelShader(ps_mask_blob->GetBufferPointer(), ps_mask_blob->GetBufferSize(), nullptr, &g_ps_mask);
        a_device->CreatePixelShader(ps_mask_skinned_blob->GetBufferPointer(), ps_mask_skinned_blob->GetBufferSize(), nullptr, &g_ps_mask_skinned);
        ps_mask_blob->Release();
        ps_mask_skinned_blob->Release();

        D3D11_BUFFER_DESC cb{};
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        cb.ByteWidth = 80;  // Mat4 + corpse_index + pad[3]（契约 v15/v18，见 PerDrawCBData）
        a_device->CreateBuffer(&cb, nullptr, &g_per_draw_cb);
        cb.ByteWidth = static_cast<UINT>(Palette_CB_Bytes);
        a_device->CreateBuffer(&cb, nullptr, &g_palette_cb);

        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_MAX;  // 诊断：并集语义，两通道剪影重叠时均保持可见
        blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MAX;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        a_device->CreateBlendState(&blend, &g_blend_mask_write);

        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = FALSE;  // 深度测试关闭 —— mask 穿墙
        a_device->CreateDepthStencilState(&depth, &g_depth_disabled);

        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;  // 剪影不受三角形绕序影响
        raster.DepthClipEnable = FALSE;
        a_device->CreateRasterizerState(&raster, &g_raster_cull_none);

        g_pipeline_ready = g_vs_static && g_vs_skinned && g_ps_mask && g_ps_mask_skinned &&
                           g_per_draw_cb && g_palette_cb && g_blend_mask_write &&
                           g_depth_disabled && g_raster_cull_none;
        if (!g_pipeline_ready)
        {
            release_pipeline();
            g_pipeline_failed = true;
            logger::error("outline mask pipeline creation failed, mask rendering disabled");
            return false;
        }

        // ---- 合成（silhouette 模式的内部填充）对象：失败只禁用叠加，不影响 mask 渲染 ----
        g_composite_ready = false;
        ID3DBlob* vs_composite_blob = compile(Vs_Composite_Source, "vs_5_0", "outline mask composite");
        ID3DBlob* ps_composite_blob = compile(Ps_Composite_Source, "ps_5_0", "outline mask composite");
        if (vs_composite_blob && ps_composite_blob)
        {
            a_device->CreateVertexShader(vs_composite_blob->GetBufferPointer(), vs_composite_blob->GetBufferSize(), nullptr, &g_vs_composite);
            a_device->CreatePixelShader(ps_composite_blob->GetBufferPointer(), ps_composite_blob->GetBufferSize(), nullptr, &g_ps_composite);

            blend.RenderTarget[0].BlendEnable = TRUE;
            blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
            blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            a_device->CreateBlendState(&blend, &g_blend_premul_alpha);

            D3D11_SAMPLER_DESC sampler{};
            sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler.MaxLOD = D3D11_FLOAT32_MAX;
            a_device->CreateSamplerState(&sampler, &g_sampler_mask);

            // b0：float4（OutlineColor rgb + Silhouette_Fill_Alpha）
            D3D11_BUFFER_DESC ccb{};
            ccb.Usage = D3D11_USAGE_DYNAMIC;
            ccb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            ccb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            ccb.ByteWidth = 16;
            a_device->CreateBuffer(&ccb, nullptr, &g_composite_cb);

            // R-03（契约 v18）：alpha LUT CB（composite/outline 共用，PS b1）
            D3D11_BUFFER_DESC lcb{};
            lcb.Usage = D3D11_USAGE_DYNAMIC;
            lcb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            lcb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            lcb.ByteWidth = static_cast<UINT>(Alpha_Lut_CB_Bytes);
            a_device->CreateBuffer(&lcb, nullptr, &g_alpha_lut_cb);

            g_composite_ready = g_vs_composite && g_ps_composite && g_blend_premul_alpha && g_sampler_mask && g_composite_cb && g_alpha_lut_cb;
        }
        if (vs_composite_blob)
            vs_composite_blob->Release();
        if (ps_composite_blob)
            ps_composite_blob->Release();
        if (!g_composite_ready)
            logger::warn("outline mask composite pipeline unavailable, debug overlay disabled (mask rendering stays active)");

        // ---- 描边 pass（契约 v12）对象：失败只禁描边，不影响 mask 渲染与叠加 ----
        // 复用 composite 的全屏三角形 VS 与预乘 alpha 混合/深度/光栅化/采样器状态。
        g_outline_ready = false;
        if (g_composite_ready)
        {
            ID3DBlob* ps_outline_blob = compile(Ps_Outline_Source, "ps_5_0", "outline mask outline");
            if (ps_outline_blob)
            {
                a_device->CreatePixelShader(ps_outline_blob->GetBufferPointer(), ps_outline_blob->GetBufferSize(), nullptr, &g_ps_outline);
                ps_outline_blob->Release();
            }

            // b0：float2 texel + float radius + float pad（16 字节）+ float4 color（16 字节）
            D3D11_BUFFER_DESC ocb{};
            ocb.Usage = D3D11_USAGE_DYNAMIC;
            ocb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            ocb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            ocb.ByteWidth = 32;
            a_device->CreateBuffer(&ocb, nullptr, &g_outline_cb);

            g_outline_ready = g_ps_outline && g_outline_cb && g_alpha_lut_cb;
        }
        if (!g_outline_ready)
            logger::warn("outline mask outline pass unavailable, corpse outline band disabled (mask rendering stays active)");

        logger::info("outline mask pipeline ready (palette {} bones/draw, overlay {})", Max_Palette_Bones, g_composite_ready ? "on" : "off");
        return true;
    }

    ID3D11InputLayout* get_layout(
        ID3D11Device* a_device, bool a_skinned, RE::BSGraphics::VertexDesc const& a_desc, std::uint32_t a_stride,
        DXGI_FORMAT a_position_format, std::uint32_t a_position_offset, MaskSkinLayout const* a_skin_layout)
    {
        // 位置格式/偏移：静态路径为 calibrate_position_format 结果（UNKNOWN 表示按
        // desc 推导）；蒙皮路径为 R-03 结果（绝不为 UNKNOWN）。
        DXGI_FORMAT const position_format = (a_position_format == DXGI_FORMAT_UNKNOWN) ? position_format_of(a_desc) : a_position_format;
        std::uint32_t const position_offset = (a_position_format == DXGI_FORMAT_UNKNOWN)
                                                  ? a_desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_POSITION)
                                                  : a_position_offset;
        // 蒙皮权重/索引布局（契约 v9 R-05）：由标定结果给出；静态路径无（nullptr），
        // 其键字段保持既有 skinning_offset 语义，行为不变。
        MaskSkinLayout const skin_layout = a_skin_layout ? *a_skin_layout : MaskSkinLayout{};
        LayoutKey const key{
            .skinned = a_skinned,
            .full_prec = a_desc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC),
            .position_format = static_cast<std::uint32_t>(position_format),
            .position_offset = position_offset,
            .skinning_offset = a_desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING),
            .stride = a_stride,
            .weight_format = static_cast<std::uint32_t>(skin_layout.weight_format),
            .weight_offset = skin_layout.weight_offset,
            .index_format = static_cast<std::uint32_t>(skin_layout.index_format),
            .index_offset = skin_layout.index_offset,
        };

        for (auto const& [cached, layout] : g_layout_cache)
        {
            if (cached == key)
                return layout;
        }

        D3D11_INPUT_ELEMENT_DESC elements[3]{};
        UINT count = 0;
        elements[count++] = {
            .SemanticName = "POSITION",
            .SemanticIndex = 0,
            .Format = position_format,
            .InputSlot = 0,
            .AlignedByteOffset = position_offset,
            .InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA,
            .InstanceDataStepRate = 0
        };
        if (a_skinned)
        {
            // SKINNING 块内布局由 R-04 自标定给出（权重/索引的格式与字节偏移），
            // 语义名顺序（BLENDWEIGHT / BLENDINDICES）与 Vs_Skinned_Source 一致。
            elements[count++] = {
                .SemanticName = "BLENDWEIGHT",
                .SemanticIndex = 0,
                .Format = skin_layout.weight_format,
                .InputSlot = 0,
                .AlignedByteOffset = skin_layout.weight_offset,
                .InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA,
                .InstanceDataStepRate = 0
            };
            elements[count++] = {
                .SemanticName = "BLENDINDICES",
                .SemanticIndex = 0,
                .Format = skin_layout.index_format,
                .InputSlot = 0,
                .AlignedByteOffset = skin_layout.index_offset,
                .InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA,
                .InstanceDataStepRate = 0
            };
        }

        ID3DBlob* blob = a_skinned ? g_vs_skinned_blob : g_vs_static_blob;
        ID3D11InputLayout* layout = nullptr;
        HRESULT const hr = a_device->CreateInputLayout(elements, count, blob->GetBufferPointer(), blob->GetBufferSize(), &layout);
        if (FAILED(hr) || !layout)
        {
            static bool s_layout_failure_reported = false;
            if (!s_layout_failure_reported)
            {
                s_layout_failure_reported = true;
                logger::error("outline mask: CreateInputLayout failed ({:X}), affected meshes skipped", static_cast<unsigned int>(hr));
            }
            return nullptr;
        }
        g_layout_cache.emplace_back(key, layout);
        return layout;
    }

    void update_cb(ID3D11DeviceContext* a_context, ID3D11Buffer* a_cb, void const* a_data, std::size_t a_bytes)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(a_context->Map(a_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return;
        std::memcpy(mapped.pData, a_data, a_bytes);
        a_context->Unmap(a_cb, 0);
    }

    // per-draw 常量缓冲（契约 v15 R-02）：矩阵 + 逐 draw 的距离衰减 alpha。HLSL 侧
    // 声明为 `row_major float4x4 + float g_corpse_index + float3 g_pad`（packing 后同为
    // 80 字节，corpse_index 在 64 字节处）。80 为 16 的倍数，满足 CB 尺寸对齐要求。
    struct PerDrawCBData
    {
        Mat4 mvp;          // 静态=ViewProj*World，蒙皮=ViewProj；上传经 oriented()
        float corpse_index;  // 目标序号/255（契约 v18）
        float pad[3];
    };
    static_assert(sizeof(PerDrawCBData) == 80);
    static_assert(sizeof(PerDrawCBData) % 16 == 0);

    void draw_mask_pass(ID3D11Device* a_device, ID3D11DeviceContext* a_context, Mat4 const& a_view_proj, std::vector<MaskDraw> const& a_draws)
    {
        for (MaskDraw const& draw : a_draws)
        {
            if (!draw.vertex_buffer || !draw.index_buffer || draw.index_count == 0 || draw.vertex_stride == 0)
                continue;

            // 蒙皮 draw 传标定布局；静态 draw 传 nullptr（行为与既有完全一致）
            ID3D11InputLayout* layout = get_layout(a_device, draw.skinned, draw.vertex_desc, draw.vertex_stride,
                draw.position_format, draw.position_offset, draw.skinned ? &draw.skin_layout : nullptr);
            if (!layout)
                continue;

            // 诊断脚手架：k_diag_bind_pose 时蒙皮几何用绑定姿态（rootParent 世界矩阵、
            // 静态 VS，不读调色板），隔离“调色板数学”与“步进/数量/投影”两类错误
            bool const use_palette = draw.skinned && !k_diag_bind_pose;
            ID3D11VertexShader* vs = use_palette ? g_vs_skinned : g_vs_static;
            ID3D11PixelShader* ps = draw.skinned ? g_ps_mask_skinned : g_ps_mask;  // 诊断：通道编码
            if (!vs || !ps)
                continue;

            // b0：静态/绑定姿态 = ViewProj × 世界变换；调色板蒙皮 = ViewProj（世界变换在调色板里）
            Mat4 per_draw;
            if (use_palette)
            {
                per_draw = a_view_proj;
            }
            else if (draw.skinned)
            {
                RE::NiSkinInstance* skin = draw.skin.get();
                RE::NiAVObject* root_parent = skin ? skin->rootParent : nullptr;
                if (!root_parent)
                {
                    logger::debug("outline mask: skinned geometry without rootParent, skipped");
                    continue;
                }
                per_draw = a_view_proj * Mat4::from_transform(root_parent->world);
            }
            else
            {
                per_draw = a_view_proj * Mat4::from_transform(draw.node->world);
            }
            // R-02（契约 v15）：oriented() 只作用矩阵部分（转置上传路径行为不变），
            // mask alpha 原样随 CB 传入 VS
            PerDrawCBData cb_data{};
            cb_data.mvp = oriented(per_draw);
            cb_data.corpse_index = draw.corpse_index;
            update_cb(a_context, g_per_draw_cb, &cb_data, sizeof(cb_data));
            a_context->VSSetConstantBuffers(0, 1, &g_per_draw_cb);

            if (use_palette)
            {
                RE::NiSkinInstance* skin = draw.skin.get();

                // 调色板：palette[i] = from_transform(boneWorld[i]) × from_transform(skinToBone(i))。
                // 消费约定实证（诊断迭代 3 world-variant）：NiTransform 原样消费
                // （from_transform 不转置）即引擎语义。引擎蒙皮组合为 skin→bone→world
                // （先 StB 后 BW），其列向量矩阵为 BW_col·StB_col——与引擎行向量记法
                // v·StB·BW 的列形式 (StB·BW)ᵀ = BWᵀ·StBᵀ 相一致（M_col(X) = X 原样
                // 存储），故两因子相乘的次序保持 boneWorld 在前。
                // 契约 v11 R-01：调色板按**全局骨骼索引空间**构建（顶点索引即 skin 骨骼
                // 数组下标），P = min(skinData 骨骼数, numMatrices) 为其有效长度；不再使用
                // part.bones（分区局部）。未用槽位填充 palette[P-1] 的副本（防越界读取未
                // 定义内容），整块上传 Max_Palette_Bones 个矩阵（v10 行为保留）。
                std::uint32_t const skin_bones = skin->skinData->GetBoneCount();
                std::uint32_t const matrix_count = skin->numMatrices;
                std::uint32_t const palette_count = palette_slot_count(skin);
                if (palette_count == 0 || palette_count > Max_Palette_Bones)
                {
                    log_skinned_skip_once(true, draw.node ? draw.node->name.c_str() : nullptr, "palette slot count out of range",
                        fmt::format("partition={} P={} skin_bones={} numMatrices={} budget={}",
                            draw.partition, palette_count, skin_bones, matrix_count, Max_Palette_Bones));
                    continue;
                }

                Mat4 palette[Max_Palette_Bones];
                bool palette_ok = true;
                for (std::uint32_t i = 0; i < palette_count; ++i)
                {
                    // i < numMatrices 与 i < GetBoneCount() 由 P 的定义保证（防越界读取）
                    if (!skin->boneWorldTransforms[i])
                    {
                        palette_ok = false;
                        break;
                    }
                    palette[i] = Mat4::from_transform(*skin->boneWorldTransforms[i]) *
                                 Mat4::from_transform(skin->skinData->GetBoneDataSkinToBone(i));
                }
                if (!palette_ok)
                {
                    log_skinned_skip_once(true, draw.node ? draw.node->name.c_str() : nullptr, "null bone world transform in palette range",
                        fmt::format("partition={} P={} skin_bones={} numMatrices={}", draw.partition, palette_count, skin_bones, matrix_count));
                    continue;
                }
                for (std::size_t k = palette_count; k < Max_Palette_Bones; ++k)
                    palette[k] = palette[palette_count - 1];
                std::size_t const palette_bytes = Max_Palette_Bones * sizeof(Mat4);
                if (g_mvp_upload_transposed)
                {
                    Mat4 palette_upload[Max_Palette_Bones];
                    for (std::size_t k = 0; k < Max_Palette_Bones; ++k)
                        palette_upload[k] = palette[k].transposed();
                    update_cb(a_context, g_palette_cb, palette_upload, palette_bytes);
                }
                else
                {
                    update_cb(a_context, g_palette_cb, palette, palette_bytes);
                }
                a_context->VSSetConstantBuffers(1, 1, &g_palette_cb);
            }

            a_context->IASetInputLayout(layout);
            a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            UINT const stride = draw.vertex_stride;
            UINT const offset = 0;
            ID3D11Buffer* vb = draw.vertex_buffer;
            a_context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
            // BSTriShape::vertexCount / 分区 vertices 均为 uint16_t：索引恒为 16 位
            a_context->IASetIndexBuffer(draw.index_buffer, DXGI_FORMAT_R16_UINT, 0);
            a_context->VSSetShader(vs, nullptr, 0);
            a_context->PSSetShader(ps, nullptr, 0);
            a_context->DrawIndexed(draw.index_count, 0, 0);
        }
    }

    void draw_composite(ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_target)
    {
        // 合成对象缺失（创建失败）时静默跳过：只影响调试叠加，不影响 mask 渲染。
        // g_mask_srv 为空时若继续绘制，采样的将是未绑定 SRV（默认值 alpha=1，
        // 表现为整屏均匀着色）——同样必须跳过。
        if (!a_target || !g_mask_srv || !g_vs_composite || !g_ps_composite || !g_blend_premul_alpha || !g_sampler_mask || !g_composite_cb || !g_alpha_lut_cb)
            return;

        // ---- R-01（契约 v14）：每帧填充 composite 常量缓冲（rgb = OutlineColor 解码，
        // a = Silhouette_Fill_Alpha 0.5），单色填充静态/蒙皮剪影。----
        std::uint32_t const rgb = Setting::get_config().outline_color;
        float const cb_data[4] = {
            static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
            static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
            static_cast<float>(rgb & 0xFF) / 255.0f,
            Silhouette_Fill_Alpha,
        };

        D3D11_VIEWPORT const vp{ 0.0f, 0.0f, static_cast<float>(g_mask_w), static_cast<float>(g_mask_h), 0.0f, 1.0f };
        a_context->OMSetRenderTargets(1, &a_target, nullptr);
        a_context->OMSetBlendState(g_blend_premul_alpha, nullptr, 0xFFFFFFFF);
        a_context->OMSetDepthStencilState(g_depth_disabled, 0);
        a_context->RSSetState(g_raster_cull_none);
        a_context->RSSetViewports(1, &vp);
        a_context->IASetInputLayout(nullptr);  // SV_VertexID 全屏三角形，无需布局
        a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer* const no_vb = nullptr;
        UINT const zero = 0;
        a_context->IASetVertexBuffers(0, 1, &no_vb, &zero, &zero);
        a_context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
        a_context->VSSetShader(g_vs_composite, nullptr, 0);
        a_context->PSSetShader(g_ps_composite, nullptr, 0);
        a_context->PSSetShaderResources(0, 1, &g_mask_srv);
        a_context->PSSetSamplers(0, 1, &g_sampler_mask);
        // PS 常量缓冲槽 b0（composite CB）+ b1（alpha LUT，契约 v18）被本 pass 改写——
        // 两槽一起就地保存/恢复（既有 restore 路径不覆盖 PS 常量缓冲）
        ID3D11Buffer* prev_ps_cbs[2] = {};
        a_context->PSGetConstantBuffers(0, 2, prev_ps_cbs);
        ID3D11Buffer* const ps_cbs[2] = { g_composite_cb, g_alpha_lut_cb };
        update_cb(a_context, g_composite_cb, cb_data, sizeof(cb_data));
        a_context->PSSetConstantBuffers(0, 2, ps_cbs);
        a_context->Draw(3, 0);
        a_context->PSSetConstantBuffers(0, 2, prev_ps_cbs);
        for (ID3D11Buffer* cb : prev_ps_cbs)
        {
            if (cb)
                cb->Release();  // Get 系列返回已 AddRef 的接口（同文件 restore 块的处理惯例）
        }
    }

    // 描边常量缓冲（契约 v12 R-01 布局，16 字节对齐）：texel/radius/pad + color
    struct OutlineCBData
    {
        float texel_x;
        float texel_y;
        float radius;
        float pad;
        float color_r;
        float color_g;
        float color_b;
        float color_a;
    };
    static_assert(sizeof(OutlineCBData) == 32);

    void draw_outline(ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_target)
    {
        // 描边对象缺失（创建失败）时静默跳过：只影响描边带，不影响 mask 渲染与叠加。
        if (!a_target || !g_mask_srv || !g_ps_outline || !g_outline_cb || !g_outline_ready || !g_alpha_lut_cb)
            return;

        // ---- R-04：每帧填充描边常量缓冲。半径 = clamp(round(OutlineThickness), 1, 6)
        //（上限控制 PS 采样数 (2r+1)² ≤ 169）；被 clamp 时一次性 INFO。颜色解码同
        // renderer.cpp 的移位先例；alpha 恒 1.0（按距离淡出留待后续修订）。----
        Config const& cfg = Setting::get_config();
        int const thickness_rounded = static_cast<int>(std::lround(cfg.outline_thickness));
        int const radius = std::clamp(thickness_rounded, 1, static_cast<int>(Max_Outline_Radius));
        static bool s_radius_clamp_reported = false;
        if (!s_radius_clamp_reported &&
            (thickness_rounded < 1 || thickness_rounded > static_cast<int>(Max_Outline_Radius)))
        {
            s_radius_clamp_reported = true;
            logger::info("outline mask: outline thickness {} clamped to {} pixels (max keeps the dilate pass at {} taps)",
                thickness_rounded, radius, (2 * Max_Outline_Radius + 1) * (2 * Max_Outline_Radius + 1));
        }
        std::uint32_t const rgb = cfg.outline_color;
        OutlineCBData const cb_data{
            .texel_x = 1.0f / static_cast<float>(g_mask_w),
            .texel_y = 1.0f / static_cast<float>(g_mask_h),
            .radius = static_cast<float>(radius),
            .pad = 0.0f,
            .color_r = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
            .color_g = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
            .color_b = static_cast<float>(rgb & 0xFF) / 255.0f,
            .color_a = Outline_Alpha,
        };

        D3D11_VIEWPORT const vp{ 0.0f, 0.0f, static_cast<float>(g_mask_w), static_cast<float>(g_mask_h), 0.0f, 1.0f };
        a_context->OMSetRenderTargets(1, &a_target, nullptr);
        a_context->OMSetBlendState(g_blend_premul_alpha, nullptr, 0xFFFFFFFF);
        a_context->OMSetDepthStencilState(g_depth_disabled, 0);
        a_context->RSSetState(g_raster_cull_none);
        a_context->RSSetViewports(1, &vp);
        a_context->IASetInputLayout(nullptr);  // SV_VertexID 全屏三角形，无需布局
        a_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer* const no_vb = nullptr;
        UINT const zero = 0;
        a_context->IASetVertexBuffers(0, 1, &no_vb, &zero, &zero);
        a_context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
        a_context->VSSetShader(g_vs_composite, nullptr, 0);
        a_context->PSSetShader(g_ps_outline, nullptr, 0);
        a_context->PSSetShaderResources(0, 1, &g_mask_srv);
        a_context->PSSetSamplers(0, 1, &g_sampler_mask);
        // PS 常量缓冲槽 b0（描边 CB）+ b1（alpha LUT，契约 v18）被本 pass 改写——
        // 两槽一起就地保存/恢复（既有 restore 路径不覆盖 PS 常量缓冲）
        ID3D11Buffer* prev_ps_cbs[2] = {};
        a_context->PSGetConstantBuffers(0, 2, prev_ps_cbs);
        ID3D11Buffer* const ps_cbs[2] = { g_outline_cb, g_alpha_lut_cb };
        update_cb(a_context, g_outline_cb, &cb_data, sizeof(cb_data));
        a_context->VSSetConstantBuffers(0, 1, &g_outline_cb);
        a_context->PSSetConstantBuffers(0, 2, ps_cbs);
        a_context->Draw(3, 0);
        a_context->PSSetConstantBuffers(0, 2, prev_ps_cbs);
        for (ID3D11Buffer* cb : prev_ps_cbs)
        {
            if (cb)
                cb->Release();  // Get 系列返回已 AddRef 的接口（同文件 restore 块的处理惯例）
        }
    }

    void render_impl(ID3D11Device* a_device, ID3D11DeviceContext* a_context, RE::NiCamera* a_camera, std::uint32_t a_width, std::uint32_t a_height)
    {
        if (!a_device || !a_context || !a_camera || a_width == 0 || a_height == 0)
            return;

        std::vector<MaskTarget> targets;
        {
            std::lock_guard<std::mutex> const lock(g_target_mutex);
            targets = g_targets;
        }

        if (targets.empty())
        {
            // 无目标：清掉旧 mask，避免 get_mask_srv 读到陈旧内容
            if (g_mask_rtv)
                a_context->ClearRenderTargetView(g_mask_rtv, Mask_Clear_Color);
            return;
        }

        if (!ensure_mask_target(a_device, a_width, a_height) || !ensure_mask_pipeline(a_device))
            return;

        std::vector<MaskDraw> draws;
        collect_draws(targets, draws);
        if (draws.empty())
        {
            a_context->ClearRenderTargetView(g_mask_rtv, Mask_Clear_Color);
            return;
        }

        // ---- 诊断脚手架：首个有 draw 的帧转储前 8 条记录（一次性 INFO）----
        // 若仍出现整屏覆盖垃圾，此转储直接点名 exploding 的几何体（RTTI/节点名/
        // 描述符标志/步进/数量/世界平移）。
        static bool s_record_dump_done = false;
        if (!s_record_dump_done)
        {
            s_record_dump_done = true;
            std::size_t const dump_count = std::min<std::size_t>(draws.size(), 8);
            for (std::size_t i = 0; i < dump_count; ++i)
            {
                MaskDraw const& d = draws[i];
                char const* rtti_name = d.node && d.node->GetRTTI() ? d.node->GetRTTI()->GetName() : "?";
                char const* node_name = d.node ? d.node->name.c_str() : nullptr;
                RE::NiPoint3 const world_t = d.skinned
                                                  ? (d.skin.get() && d.skin.get()->rootParent ? d.skin.get()->rootParent->world.translate : RE::NiPoint3{ 0.0f, 0.0f, 0.0f })
                                                  : (d.node ? d.node->world.translate : RE::NiPoint3{ 0.0f, 0.0f, 0.0f });
                logger::info(
                    "outline mask record[{}]: rtti={} node=\"{}\" skinned={} flags={:#06x} stride={} verts={} tris={} world=({:.1f},{:.1f},{:.1f})",
                    i,
                    rtti_name ? rtti_name : "?",
                    node_name ? node_name : "?",
                    d.skinned,
                    static_cast<unsigned>(d.vertex_desc.GetFlags()),
                    d.vertex_stride,
                    d.vertex_count,
                    d.triangle_count,
                    world_t.x, world_t.y, world_t.z);
            }
            if (draws.size() > dump_count)
                logger::info("outline mask: {} more draw records not dumped", draws.size() - dump_count);
        }

        // ---- 诊断脚手架：地面真值校验（首个有 draw 的帧一次，INFO）----
        // 锚点 = 首记录节点的世界原点（model 的第 4 列）。经组合矩阵投影与引擎
        // WorldPtToScreenPt3（线框路径同款）像素比对：五点解算实证直传应达
        // ~1e-6（亚像素）；若直传失败，自动尝试转置字节序上传一次并记录所选形式。
        Mat4 const view_proj = Mat4::from_world_to_cam_raw(a_camera->GetRuntimeData().worldToCam);

        static bool s_ground_truth_checked = false;
        if (!s_ground_truth_checked && draws.front().node)
        {
            RE::NiPoint3 const anchor = draws.front().node->world.translate;
            Mat4 const model = Mat4::from_transform(draws.front().node->world);
            Mat4 const mvp = view_proj * model;

            // 引擎像素：左下原点归一化输出；port 为像素单位时先归一化（同 renderer.cpp）
            struct PortRect
            {
                float left, right, top, bottom;
            };
            static_assert(sizeof(PortRect) == sizeof(RE::NiRect<float>));
            PortRect pr{};
            std::memcpy(&pr, &a_camera->GetRuntimeData2().port, sizeof(pr));
            float ex = 0.0f, ey = 0.0f, ez = 0.0f;
            bool const engine_ok = a_camera->WorldPtToScreenPt3(anchor, ex, ey, ez, 1e-5f);
            float nx = ex;
            float ny = ey;
            if (pr.right - pr.left > 10.0f)
                nx = (ex - pr.left) / (pr.right - pr.left);
            if (pr.bottom - pr.top > 10.0f)
                ny = (ey - pr.top) / (pr.bottom - pr.top);
            float const eng_px = nx * static_cast<float>(a_width);
            float const eng_py = (1.0f - ny) * static_cast<float>(a_height);

            // 直传：局部原点 (0,0,0,1) 的裁剪坐标 = mvp 第 4 列；转置上传：= mvp 第 3 行
            float const cw = mvp.m[3][3];
            if (engine_ok && cw > 1e-5f)
            {
                s_ground_truth_checked = true;
                float const px_d = ((mvp.m[0][3] / cw) * 0.5f + 0.5f) * static_cast<float>(a_width);
                float const py_d = (1.0f - ((mvp.m[1][3] / cw) * 0.5f + 0.5f)) * static_cast<float>(a_height);
                float const px_t = ((mvp.m[3][0] / cw) * 0.5f + 0.5f) * static_cast<float>(a_width);
                float const py_t = (1.0f - ((mvp.m[3][1] / cw) * 0.5f + 0.5f)) * static_cast<float>(a_height);
                float const err_d = std::fabs(px_d - eng_px) + std::fabs(py_d - eng_py);
                float const err_t = std::fabs(px_t - eng_px) + std::fabs(py_t - eng_py);
                g_mvp_upload_transposed = err_t < err_d;
                float const composed_px = g_mvp_upload_transposed ? px_t : px_d;
                float const composed_py = g_mvp_upload_transposed ? py_t : py_d;
                logger::info(
                    "outline mask: ground-truth delta direct=({:.4f},{:.4f})px transposed-upload=({:.4f},{:.4f})px "
                    "-> upload {} anchor engine=({:.2f},{:.2f}) composed=({:.2f},{:.2f})",
                    px_d - eng_px, py_d - eng_py, px_t - eng_px, py_t - eng_py,
                    g_mvp_upload_transposed ? "transposed" : "direct",
                    eng_px, eng_py, composed_px, composed_py);
            }
            // 锚点在相机后方/引擎投影失败：下一帧重试（不置位）
        }

        // ---- 保存游戏渲染状态（同 renderer.cpp 的保存/恢复模式，扩展到本类触及的管线段）----
        ID3D11RenderTargetView* prev_rtv = nullptr;
        ID3D11DepthStencilView* prev_dsv = nullptr;
        a_context->OMGetRenderTargets(1, &prev_rtv, &prev_dsv);

        ID3D11BlendState* prev_blend = nullptr;
        float blend_factor[4]{};
        UINT sample_mask = 0;
        a_context->OMGetBlendState(&prev_blend, blend_factor, &sample_mask);

        ID3D11DepthStencilState* prev_depth = nullptr;
        UINT prev_stencil = 0;
        a_context->OMGetDepthStencilState(&prev_depth, &prev_stencil);

        ID3D11RasterizerState* prev_rs = nullptr;
        a_context->RSGetState(&prev_rs);

        UINT prev_vp_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        D3D11_VIEWPORT prev_viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        a_context->RSGetViewports(&prev_vp_count, prev_viewports);

        ID3D11InputLayout* prev_layout = nullptr;
        a_context->IAGetInputLayout(&prev_layout);

        D3D11_PRIMITIVE_TOPOLOGY prev_topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        a_context->IAGetPrimitiveTopology(&prev_topology);

        ID3D11Buffer* prev_vb = nullptr;
        UINT prev_vb_stride = 0;
        UINT prev_vb_offset = 0;
        a_context->IAGetVertexBuffers(0, 1, &prev_vb, &prev_vb_stride, &prev_vb_offset);

        ID3D11Buffer* prev_ib = nullptr;
        DXGI_FORMAT prev_ib_format = DXGI_FORMAT_UNKNOWN;
        UINT prev_ib_offset = 0;
        a_context->IAGetIndexBuffer(&prev_ib, &prev_ib_format, &prev_ib_offset);

        ID3D11VertexShader* prev_vs = nullptr;
        ID3D11ClassInstance* prev_vs_instances[8]{};
        UINT prev_vs_instance_count = 8;
        a_context->VSGetShader(&prev_vs, prev_vs_instances, &prev_vs_instance_count);
        ID3D11PixelShader* prev_ps = nullptr;
        ID3D11ClassInstance* prev_ps_instances[8]{};
        UINT prev_ps_instance_count = 8;
        a_context->PSGetShader(&prev_ps, prev_ps_instances, &prev_ps_instance_count);

        ID3D11Buffer* prev_vs_cbs[2] = {};
        a_context->VSGetConstantBuffers(0, 2, prev_vs_cbs);

        ID3D11ShaderResourceView* prev_srv = nullptr;
        a_context->PSGetShaderResources(0, 1, &prev_srv);
        ID3D11SamplerState* prev_sampler = nullptr;
        a_context->PSGetSamplers(0, 1, &prev_sampler);

        // ---- mask pass（契约 v18 R-01/R-02）：一次清屏，逐 draw 绘制全部目标；
        // 每个 draw 经 per-draw CB 携带尸体索引，mask PS 写入 B 通道（MAX 混合在
        // 重叠区取较高索引）----
        a_context->OMSetRenderTargets(1, &g_mask_rtv, nullptr);
        a_context->ClearRenderTargetView(g_mask_rtv, Mask_Clear_Color);
        a_context->OMSetBlendState(g_blend_mask_write, nullptr, 0xFFFFFFFF);
        a_context->OMSetDepthStencilState(g_depth_disabled, 0);
        a_context->RSSetState(g_raster_cull_none);

        D3D11_VIEWPORT const vp{ 0.0f, 0.0f, static_cast<float>(g_mask_w), static_cast<float>(g_mask_h), 0.0f, 1.0f };
        a_context->RSSetViewports(1, &vp);

        draw_mask_pass(a_device, a_context, view_proj, draws);

        // ---- R-03（契约 v18）：per-frame alpha LUT（与目标快照同序，其余槽位 0），
        // 消费 PS 以 mask B 通道的尸体索引查表——单次消费无复合，重叠像素取较高
        // 索引尸体的 alpha（常量），每具尸体各部位颜色一致。----
        Config::DisplayMode const display_mode = Setting::get_config().display_mode;
        bool const consumer_active =
            (display_mode == Config::DisplayMode::e_silhouette && g_composite_ready) ||
            (display_mode == Config::DisplayMode::e_outline && g_outline_ready);
        if (consumer_active && g_alpha_lut_cb)
        {
            float alpha_lut[Alpha_Lut_Floats]{};
            std::size_t const lut_count = std::min<std::size_t>(targets.size(), Alpha_Lut_Floats);
            for (std::size_t i = 0; i < lut_count; ++i)
                alpha_lut[i] = targets[i].opacity;
            update_cb(a_context, g_alpha_lut_cb, alpha_lut, sizeof(alpha_lut));

            // 显示模式门控（契约 v13/v18）：silhouette=内部填充叠加（draw_composite）、
            // outline=外描边带（draw_outline），各只调用一次；icon 模式两 pass 均不画
            //（防御：正常路径 renderer.cpp 在 icon 模式不会调用本类）。
            if (display_mode == Config::DisplayMode::e_silhouette && prev_rtv)
                draw_composite(a_context, prev_rtv);
            else if (display_mode == Config::DisplayMode::e_outline && prev_rtv)
                draw_outline(a_context, prev_rtv);
        }

        // ---- 恢复游戏渲染状态 ----
        a_context->OMSetRenderTargets(1, &prev_rtv, prev_dsv);
        a_context->OMSetBlendState(prev_blend, blend_factor, sample_mask);
        a_context->OMSetDepthStencilState(prev_depth, prev_stencil);
        a_context->RSSetState(prev_rs);
        a_context->RSSetViewports(prev_vp_count, prev_viewports);
        a_context->IASetInputLayout(prev_layout);
        a_context->IASetPrimitiveTopology(prev_topology);
        a_context->IASetVertexBuffers(0, 1, &prev_vb, &prev_vb_stride, &prev_vb_offset);
        a_context->IASetIndexBuffer(prev_ib, prev_ib_format, prev_ib_offset);
        a_context->VSSetShader(prev_vs, prev_vs_instances, prev_vs_instance_count);
        a_context->PSSetShader(prev_ps, prev_ps_instances, prev_ps_instance_count);
        a_context->VSSetConstantBuffers(0, 2, prev_vs_cbs);
        a_context->PSSetShaderResources(0, 1, &prev_srv);
        a_context->PSSetSamplers(0, 1, &prev_sampler);

        // D3D11 的 Get 系列在默认/隐式状态被绑定时返回 nullptr（如无 DSV、默认
        // blend/depth/raster/layout），Release 前必须判空——与 renderer.cpp 的恢复
        // 块同一处理；空指针虚调用 Release 即访问违例（且无法被 try/catch 捕获）。
        if (prev_rtv)
            prev_rtv->Release();
        if (prev_dsv)
            prev_dsv->Release();
        if (prev_blend)
            prev_blend->Release();
        if (prev_depth)
            prev_depth->Release();
        if (prev_rs)
            prev_rs->Release();
        if (prev_layout)
            prev_layout->Release();
        if (prev_vb)
            prev_vb->Release();
        if (prev_ib)
            prev_ib->Release();
        if (prev_vs)
            prev_vs->Release();
        if (prev_ps)
            prev_ps->Release();
        for (ID3D11ClassInstance* inst : prev_vs_instances)
        {
            if (inst)
                inst->Release();
        }
        for (ID3D11ClassInstance* inst : prev_ps_instances)
        {
            if (inst)
                inst->Release();
        }
        for (ID3D11Buffer* cb : prev_vs_cbs)
        {
            if (cb)
                cb->Release();
        }
        if (prev_srv)
            prev_srv->Release();
        if (prev_sampler)
            prev_sampler->Release();
    }
}

void OutlineMask::set_targets(std::vector<OutlineMaskTarget> const& a_targets)
{
    std::vector<MaskTarget> kept;
    kept.reserve(a_targets.size());
    for (OutlineMaskTarget const& target : a_targets)
    {
        if (target.ref)
            kept.push_back(MaskTarget{ RE::NiPointer<RE::TESObjectREFR>(target.ref), target.opacity });  // NiPointer 构造即保活
    }

    std::lock_guard<std::mutex> const lock(g_target_mutex);
    g_targets = std::move(kept);
}

void OutlineMask::render(ID3D11Device* a_device, ID3D11DeviceContext* a_context, RE::NiCamera* a_camera, std::uint32_t a_width, std::uint32_t a_height)
{
    // Present 回调边界内禁止异常外泄：任何未预期失败记日志并跳过本帧
    try
    {
        render_impl(a_device, a_context, a_camera, a_width, a_height);
    }
    catch (std::exception const& e)
    {
        logger::error("outline mask: frame skipped ({})", e.what());
    }
    catch (...)
    {
        logger::error("outline mask: frame skipped (unexpected error)");
    }
}

ID3D11ShaderResourceView* OutlineMask::get_mask_srv(std::uint32_t& a_width, std::uint32_t& a_height)
{
    a_width = g_mask_w;
    a_height = g_mask_h;
    return g_mask_srv;
}

PLUGIN_NAMESPACE_END
