//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

#include <cstdint>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;

namespace RE
{
    class NiCamera;
    class TESObjectREFR;
}

PLUGIN_NAMESPACE_BEGIN

// mask 渲染目标：引用 + 距离衰减不透明度
//（corpse_alpha(distance)，按目标序号填入消费 pass 的 per-frame alpha LUT）。
struct OutlineMaskTarget
{
    RE::TESObjectREFR* ref;
    float opacity;
};

// 可搜刮尸体的 mesh 描边 mask 渲染器（门面）。
//
// 每帧把 set_targets 传入的目标对象的 3D mesh（含 GPU 蒙皮）绘制进一张离屏
// mask RT（RGBA8_UNORM，无深度缓冲），渲染全程不做深度测试——mask 天然穿墙。
// silhouette/outline 显示模式由本类按 display_mode 选择消费 pass（内部填充叠加 /
// 外描边带）。
//
// 线程模型：set_targets / render 均在渲染线程（Present 回调）调用；目标列表以
// RE::NiPointer 保活并用互斥锁保护。
class OutlineMask
{
public:
    OutlineMask() = delete;
    ~OutlineMask() = delete;
    OutlineMask(OutlineMask const&) = delete;
    OutlineMask(OutlineMask const&&) = delete;
    OutlineMask operator=(OutlineMask&) = delete;
    OutlineMask operator=(OutlineMask&&) = delete;

    static void set_targets(std::vector<OutlineMaskTarget> const& a_targets);

    // 由 Present 回调调用：收集 a_camera 视角下的几何并渲染 mask，随后按当前
    // display_mode 把填充/描边叠加到 a_overlay_target 上——调用方须传 Present
    // 将展示的后备缓冲的 RTV（契约与 icon 路径一致，不依赖引擎 Present 时刻
    // 碰巧绑定的渲染目标）。
    // a_width/a_height 为后备缓冲尺寸（mask RT 与其同尺寸，变化时重建）。
    static void render(ID3D11Device* a_device, ID3D11DeviceContext* a_context, RE::NiCamera* a_camera,
        ID3D11RenderTargetView* a_overlay_target, std::uint32_t a_width, std::uint32_t a_height);
};

PLUGIN_NAMESPACE_END
