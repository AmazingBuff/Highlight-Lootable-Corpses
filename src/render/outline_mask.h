#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace RE
{
	class NiCamera;
	class TESObjectREFR;
}

PLUGIN_NAMESPACE_BEGIN

// 可搜刮尸体的 mesh 描边 mask 渲染器。
//
// 每帧把 set_targets 传入的目标对象的 3D mesh（含 GPU 蒙皮）绘制进一张离屏
// mask RT（RGBA8_UNORM，无深度缓冲），渲染全程不做深度测试——mask 天然穿墙。
// 对外通过 get_mask_srv() 输出 mask 纹理（SRV），供后续边缘检测/描边 pass 消费；
// OutlineMaskDebug 开启时在本帧画面上叠加半透明 mask 便于人工验证。
//
// 线程模型：set_targets / render / get_mask_srv 均在渲染线程（Present 回调）调用；
// 目标列表以 RE::NiPointer 保活并用互斥锁保护。
class OutlineMask
{
public:
    OutlineMask() = delete;
    ~OutlineMask() = delete;
    OutlineMask(OutlineMask const&) = delete;
    OutlineMask(OutlineMask const&&) = delete;
    OutlineMask operator=(OutlineMask&) = delete;
    OutlineMask operator=(OutlineMask&&) = delete;

    static void set_targets(std::vector<RE::TESObjectREFR*> const& a_targets);

    // 由 Present 回调调用：收集 a_camera 视角下的几何并渲染 mask。
    // a_width/a_height 为后备缓冲尺寸（mask RT 与其同尺寸，变化时重建）。
    static void render(ID3D11Device* a_device, ID3D11DeviceContext* a_context, RE::NiCamera* a_camera, std::uint32_t a_width, std::uint32_t a_height);

    // 返回本帧 mask 纹理的 SRV（未创建时为 nullptr）与尺寸。
    [[nodiscard]] static ID3D11ShaderResourceView* get_mask_srv(std::uint32_t& a_width, std::uint32_t& a_height);
};

PLUGIN_NAMESPACE_END
