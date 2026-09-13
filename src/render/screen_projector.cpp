//
// Created by AmazingBuff on 2026/9/13.
//

#include "screen_projector.h"

#include <DirectXMath.h>

#include <RE/Skyrim.h>

#include <cmath>
#include <cstring>

PLUGIN_NAMESPACE_BEGIN

namespace
{
    // NiRect<T> 成员为 protected，按固定布局（left, right, top, bottom）
    // memcpy 到同布局的本地 POD 读取相机 port，避免修改三方库。
    struct PortRect
    {
        float left, right, top, bottom;
    };
    static_assert(sizeof(PortRect) == sizeof(RE::NiRect<float>));
}

bool project_engine(RE::NiCamera* a_camera, RE::NiPoint3 const& a_point, float a_width, float a_height, float& a_px, float& a_py, float& a_depth)
{
    if (!a_camera || !a_camera->WorldPtToScreenPt3(a_point, a_px, a_py, a_depth, 1e-5f))
        return false;

    // 相机 port 若是像素单位，先把输出归一化到 0..1
    PortRect port{};
    std::memcpy(&port, &a_camera->GetRuntimeData2().port, sizeof(port));
    float const port_w = port.right - port.left;
    float const port_h = port.bottom - port.top;
    float nx = a_px;
    float ny = a_py;
    if (port_w > 10.0f)
        nx = (a_px - port.left) / port_w;

    if (port_h > 10.0f)
        ny = (a_py - port.top) / port_h;

    // 引擎函数输出为"左下原点"归一化坐标（TrueDirectionalMovement 同样处理），翻转为左上原点
    a_px = nx * a_width;
    a_py = (1.0f - ny) * a_height;
    return true;
}

void ScreenProjector::refresh()
{
    m_camera = RE::Main::WorldRootCamera();

    // 备选：BSGraphics::State 相机缓存里的 viewProj 矩阵（AE 实测坏值，仅兜底）
    m_view_data = nullptr;
    if (RE::BSGraphics::State* state = RE::BSGraphics::State::GetSingleton())
    {
        RE::BSGraphics::State::RUNTIME_DATA& state_rt = state->GetRuntimeData();
        for (RE::BSGraphics::CameraStateData const& cam_data : state_rt.cameraDataCacheA)
        {
            if (cam_data.referenceCamera == m_camera)
            {
                m_view_data = std::addressof(cam_data.GetCameraStateRuntimeData().camViewData);
                break;
            }
        }
        if (!m_view_data && !state_rt.cameraDataCacheA.empty())
            m_view_data = std::addressof(state_rt.cameraDataCacheA.front().GetCameraStateRuntimeData().camViewData);
    }
}

bool ScreenProjector::world_to_screen(RE::NiPoint3 const& a_point, float a_width, float a_height, float& a_px, float& a_py, float& a_depth) const
{
    if (project_engine(m_camera, a_point, a_width, a_height, a_px, a_py, a_depth))
        return a_depth > 0.0f;

    if (m_view_data && (m_view_data->viewProjMatrixUnjittered._11 != 0.0f || m_view_data->viewProjMat._11 != 0.0f))
    {
        DirectX::SimpleMath::Matrix const& viewProj = m_view_data->viewProjMatrixUnjittered._11 != 0.0f ? m_view_data->viewProjMatrixUnjittered : m_view_data->viewProjMat;
        DirectX::XMVECTOR const clip = DirectX::XMVector4Transform(DirectX::XMVectorSet(a_point.x, a_point.y, a_point.z, 1.0f), viewProj);
        float const clip_w = DirectX::XMVectorGetW(clip);
        if (std::fabs(clip_w) >= 1e-5f)
        {
            DirectX::XMVECTOR const ndc = DirectX::XMVectorDivide(clip, DirectX::XMVectorReplicate(clip_w));
            float const ndc_x = DirectX::XMVectorGetX(ndc);
            float const ndc_y = DirectX::XMVectorGetY(ndc);
            float const ndc_z = DirectX::XMVectorGetZ(ndc);
            if (ndc_x >= -1.0f && ndc_x <= 1.0f && ndc_y >= -1.0f && ndc_y <= 1.0f && ndc_z >= 0.0f && ndc_z <= 1.0f)
            {
                a_px = (ndc_x * 0.5f + 0.5f) * a_width;
                a_py = (1.0f - ndc_y) * 0.5f * a_height;
                a_depth = ndc_z;
                return true;
            }
        }
    }
    return false;
}

PLUGIN_NAMESPACE_END
