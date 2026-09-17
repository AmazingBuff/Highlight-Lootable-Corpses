//
// Created by AmazingBuff on 2026/9/14.
//

#include "render_util.h"

#include "Plugin.h"

PLUGIN_NAMESPACE_BEGIN


uint32_t Color::encode() const noexcept
{
    return (static_cast<uint32_t>(m_a * 255.0f) << 24) |
           (static_cast<uint32_t>(m_r * 255.0f) << 16) |
           (static_cast<uint32_t>(m_g * 255.0f) << 8) |
            static_cast<uint32_t>(m_b * 255.0f);
}

void Color::decode(uint32_t v)
{
    m_r = static_cast<float>((v >> 16) & 0xFF) / 255.0f;
    m_g = static_cast<float>((v >> 8) & 0xFF) / 255.0f;
    m_b = static_cast<float>(v & 0xFF) / 255.0f;
    m_a = static_cast<float>((v >> 24) & 0xFF) / 255.0f;
}

bool project(RE::NiCamera* camera, RE::NiPoint3 const& point, float width, float height, float& px, float& py, float& depth)
{
    if (!camera || !camera->WorldPtToScreenPt3(point, px, py, depth, 1e-5f))
        return false;

    Rect port;
    std::memcpy(&port, &camera->GetRuntimeData2().port, sizeof(port));
    float const port_w = port.width();
    float const port_h = port.height();
    float nx = px;
    float ny = py;
    if (port_w > 10.0f)
        nx = (px - port.left()) / port_w;

    if (port_h > 10.0f)
        ny = (py - port.top()) / port_h;

    px = nx * width;
    py = (1.0f - ny) * height;
    return true;
}

bool world_to_screen(RE::NiCamera* camera, RE::BSGraphics::ViewData const* view_data, RE::NiPoint3 const& point, float width, float height, float& px, float& py, float& depth)
{
    if (project(camera, point, width, height, px, py, depth))
        return depth > 0.0f;

    if (view_data && (view_data->viewProjMatrixUnjittered._11 != 0.0f || view_data->viewProjMat._11 != 0.0f))
    {
        DirectX::SimpleMath::Matrix const& viewProj = view_data->viewProjMatrixUnjittered._11 != 0.0f ? view_data->viewProjMatrixUnjittered : view_data->viewProjMat;
        DirectX::XMVECTOR const clip = DirectX::XMVector4Transform(DirectX::XMVectorSet(point.x, point.y, point.z, 1.0f), viewProj);
        float const clip_w = DirectX::XMVectorGetW(clip);
        if (std::fabs(clip_w) >= 1e-5f)
        {
            DirectX::XMVECTOR const ndc = DirectX::XMVectorDivide(clip, DirectX::XMVectorReplicate(clip_w));
            float const ndc_x = DirectX::XMVectorGetX(ndc);
            float const ndc_y = DirectX::XMVectorGetY(ndc);
            float const ndc_z = DirectX::XMVectorGetZ(ndc);
            if (ndc_x >= -1.0f && ndc_x <= 1.0f && ndc_y >= -1.0f && ndc_y <= 1.0f && ndc_z >= 0.0f && ndc_z <= 1.0f)
            {
                px = (ndc_x * 0.5f + 0.5f) * width;
                py = (1.0f - ndc_y) * 0.5f * height;
                depth = ndc_z;
                return true;
            }
        }
    }
    return false;
}

PLUGIN_NAMESPACE_END