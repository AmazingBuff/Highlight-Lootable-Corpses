//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

PLUGIN_NAMESPACE_BEGIN

// ESP 渲染入口：安装 IDXGISwapChain::Present 钩子，每帧按 display_mode 派发
// icon（屏幕空间图元）/ silhouette（mask 内部填充）/ outline（mask 外描边带）
// 三种渲染行为（分别由 UiOverlay / OutlineMask 的消费 pass 表达）。
class Renderer
{
public:
    Renderer() = delete;
    ~Renderer() = delete;
    Renderer(Renderer const&) = delete;
    Renderer(Renderer const&&) = delete;
    Renderer operator=(Renderer&) = delete;
    Renderer operator=(Renderer&&) = delete;

    // 幂等；交换链未就绪时 WARN 并在后续游戏消息中重试。
    static void install();
};

PLUGIN_NAMESPACE_END
