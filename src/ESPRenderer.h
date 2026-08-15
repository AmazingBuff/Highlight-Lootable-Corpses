#pragma once

#include <d3d11.h>
#include <dxgi.h>

namespace ESPRenderer
{
	// 在游戏线程上调用（渲染器初始化之后），安装 IDXGISwapChain::Present 钩子；幂等
	void Install();

	// 渲染线程：每次 Present 前调用，绘制 ESP 标记
	void OnPresent(IDXGISwapChain* a_swapChain);
}
