//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace RE
{
	class NiCamera;
	class NiPoint3;
}

namespace RE::BSGraphics
{
	struct ViewData;
}

PLUGIN_NAMESPACE_BEGIN

// 引擎相机投影：NiCamera::WorldPtToScreenPt3（左下原点归一化输出）→ 左上原点像素。
// 相机 port 为像素单位时先把输出归一化到 0..1。返回引擎函数的原始 bool（不判深度）。
bool project_engine(
    RE::NiCamera* a_camera,
    RE::NiPoint3 const& a_point,
    float a_width,
    float a_height,
    float& a_px,
    float& a_py,
    float& a_depth);

// 每帧解析世界根相机（Present 时仍是本帧数据）与 BSGraphics::State 相机缓存，
// 提供世界点 → 屏幕像素投影。
class ScreenProjector
{
public:
    void refresh();

    [[nodiscard]] RE::NiCamera* camera() const { return m_camera; }

    // 引擎相机投影优先；失败时兜底 State 缓存里的 viewProj 矩阵
    // （实测在 AE 上该矩阵读出的是坏值：_22=0、_43=0，故仅作兜底保留）。
    [[nodiscard]] bool world_to_screen(
        RE::NiPoint3 const& a_point,
        float a_width,
        float a_height,
        float& a_px,
        float& a_py,
        float& a_depth) const;

private:
    RE::NiCamera* m_camera = nullptr;
    RE::BSGraphics::ViewData const* m_view_data = nullptr;
};

PLUGIN_NAMESPACE_END
