#pragma once

#include <cstdint>
#include <filesystem>

namespace Config
{
	struct Settings
	{
		bool          enabled{ true };              // 默认启用
		std::uint32_t hotkey{ 0x76 };               // F7
		float         maxDistance{ 8000.0f };       // 最大搜索距离（游戏单位，约 114 米）
		std::uint32_t scanIntervalMs{ 500 };        // 尸体扫描间隔
		std::uint32_t outlineColor{ 0x00FF66 };     // 描边颜色 (RGB)
		float         glowAlpha{ 0.30f };           // 发光强度
		float         minOpacity{ 0.15f };          // 远处标记的最小不透明度
		float         outlineThickness{ 2.0f };     // 描边线宽（像素）
		bool          showOutline{ true };          // 画包围盒描边（默认仅边框）
		bool          showGlow{ false };            // 画发光填充（默认关闭）
		bool          showCenterDot{ false };       // 画中心点（默认关闭）
		bool          showIndicator{ true };        // 画右上角开关指示点

		// 距离衰减：FadeStartDistance 内完全可见，超过后按 FadePower 指数淡出到 MinOpacity
		float         fadeStartDistance{ 1000.0f };  // 开始淡出的距离（游戏单位）
		float         fadePower{ 2.0f };             // 淡出曲线指数（越大衰减越快）
	};

	[[nodiscard]] const Settings& Get() noexcept;

	[[nodiscard]] bool IsEnabled() noexcept;
	void               SetEnabled(bool a_enabled) noexcept;
	void               SaveEnabled() noexcept;

	void                     Load() noexcept;
	[[nodiscard]] std::filesystem::path GetIniPath() noexcept;
}
