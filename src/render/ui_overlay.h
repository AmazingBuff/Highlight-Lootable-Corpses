//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

#include <vector>

#include <DirectXMath.h>

struct ID3D11Buffer;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11InputLayout;
struct ID3D11PixelShader;
struct ID3D11RenderTargetView;
struct ID3D11VertexShader;

PLUGIN_NAMESPACE_BEGIN

class OverlayStates;

// 屏幕空间彩色图元批处理（icon 模式的实心圆等），自编译 VS/PS + 自建 InputLayout/
// 动态顶点缓冲。顶点位置以像素坐标（左上原点，y 向下）输入，提交前换算为 NDC。
//
// 混合约定：OM 绑定 OverlayStates::alpha_blend()（预乘 alpha），PS 输出 rgb*a——
// 顶点颜色的 a 传直 alpha 即可。
//
// 线程模型：begin_frame/add_*/flush 须在同一线程同一段作用域内完成
// （调用方负责整体串行化，见 OverlayDirector 的绘制互斥）。
class UiOverlay
{
public:
    // 顶点缓冲容量：GPU 侧固定大小，CPU 侧提交前必须按此裁剪（否则 memcpy 越界写映射区）
    static constexpr std::size_t Vertex_Buffer_Bytes = 1024 * 1024;

    // 惰性创建管线；创建失败后不再重试（重试会持续泄漏 D3D 对象），ready 恒 false。
    void ensure(ID3D11Device* a_device);

    [[nodiscard]] bool ready() const { return m_ready; }

    // 帧开始：设定像素尺寸（add_* 以此换算 NDC）并清空累积。
    void begin_frame(float a_width, float a_height);

    // 三角扇绘制实心圆；半径为固定像素值（x/y 分别换算，保持屏幕上为圆）。
    void add_circle(float a_center_x, float a_center_y, float a_radius_px, DirectX::XMFLOAT4 const& a_color);

    // 屏幕像素坐标三角形（左上原点，y 向下）。
    void add_triangle(
        float a_x0, float a_y0,
        float a_x1, float a_y1,
        float a_x2, float a_y2,
        DirectX::XMFLOAT4 const& a_color);

    // 把本帧累积的三角形统一提交（一次 Map + 一次 Draw），绑定自身管线与状态。
    void flush(ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_target, OverlayStates const& a_states);

private:
    struct UiVertex
    {
        float x, y, z;
        float r, g, b, a;
    };

    static constexpr std::size_t Max_Vertices = Vertex_Buffer_Bytes / sizeof(UiVertex);

    bool create_pipeline(ID3D11Device* a_device);
    void release_pipeline();
    void add_vertex_ndc(float a_ndc_x, float a_ndc_y, DirectX::XMFLOAT4 const& a_color);

    ID3D11VertexShader* m_vertex_shader = nullptr;
    ID3D11PixelShader* m_pixel_shader = nullptr;
    ID3D11InputLayout* m_input_layout = nullptr;
    ID3D11Buffer* m_vertex_buffer = nullptr;
    bool m_ready = false;
    bool m_failed = false;  // 创建失败后不再每帧重试
    bool m_overflow_reported = false;

    float m_width = 0.0f;
    float m_height = 0.0f;

    std::vector<UiVertex> m_vertices;  // 一帧内累积，flush 统一提交
};

PLUGIN_NAMESPACE_END
