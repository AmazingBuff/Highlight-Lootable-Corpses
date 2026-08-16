#include "PCH.h"
#include "Config.h"

namespace
{
    Config::Settings g_settings;
    std::atomic<bool> g_enabled{ true };
}

namespace Config
{
    std::filesystem::path GetIniPath() noexcept
    {
        // 放在游戏 Data\SKSE\Plugins\ 下，与 DLL 同目录
        auto const exePath = REL::Module::get().filePath();  // SkyrimSE.exe 的完整路径（wstring_view）
        std::filesystem::path gameRoot(exePath);
        return gameRoot.parent_path() / "Data" / "SKSE" / "Plugins" / (std::string(Plugin::NAME) + ".ini");
    }

    namespace
    {
        std::uint32_t ParseHex(char const* a_value, std::uint32_t a_default) noexcept
        {
            if (!a_value || !*a_value)
            {
                return a_default;
            }
            char* end = nullptr;
            auto const value = std::strtoul(a_value, &end, 16);
            return end == a_value ? a_default : static_cast<std::uint32_t>(value);
        }
    }

    void Load() noexcept
    {
        CSimpleIniA ini;
        ini.SetUnicode();

        auto const path = GetIniPath();
        SI_Error const rc = ini.LoadFile(path.string().c_str());
        if (rc < 0)
        {
            logger::info("INI not found at {}, writing defaults", path.string());
        }

        g_settings.enabled = ini.GetBoolValue("General", "Enabled", g_settings.enabled);
        g_settings.hotkey = static_cast<std::uint32_t>(ini.GetLongValue("General", "Hotkey", static_cast<long>(g_settings.hotkey)));
        g_settings.maxDistance = static_cast<float>(ini.GetDoubleValue("General", "MaxDistance", g_settings.maxDistance));
        g_settings.scanIntervalMs = static_cast<std::uint32_t>(ini.GetLongValue("General", "ScanIntervalMs", static_cast<long>(g_settings.scanIntervalMs)));

        g_settings.outlineColor = ParseHex(ini.GetValue("Display", "OutlineColor", "00FF66"), 0x00FF66);
        g_settings.glowAlpha = static_cast<float>(ini.GetDoubleValue("Display", "GlowAlpha", g_settings.glowAlpha));
        g_settings.minOpacity = static_cast<float>(ini.GetDoubleValue("Display", "MinOpacity", g_settings.minOpacity));
        g_settings.outlineThickness = static_cast<float>(ini.GetDoubleValue("Display", "OutlineThickness", g_settings.outlineThickness));
        g_settings.showOutline = ini.GetBoolValue("Display", "ShowOutline", g_settings.showOutline);
        g_settings.showGlow = ini.GetBoolValue("Display", "ShowGlow", g_settings.showGlow);
        g_settings.showCenterDot = ini.GetBoolValue("Display", "ShowCenterDot", g_settings.showCenterDot);
        g_settings.showIndicator = ini.GetBoolValue("Display", "ShowIndicator", g_settings.showIndicator);
        g_settings.fadeStartDistance = static_cast<float>(ini.GetDoubleValue("Display", "FadeStartDistance", g_settings.fadeStartDistance));
        g_settings.fadePower = static_cast<float>(ini.GetDoubleValue("Display", "FadePower", g_settings.fadePower));

        g_enabled.store(g_settings.enabled, std::memory_order_relaxed);

        // 写回，保证文件存在且包含全部选项说明
        ini.SetBoolValue("General", "Enabled", g_settings.enabled);
        ini.SetLongValue("General", "Hotkey", static_cast<long>(g_settings.hotkey));
        ini.SetDoubleValue("General", "MaxDistance", g_settings.maxDistance);
        ini.SetLongValue("General", "ScanIntervalMs", static_cast<long>(g_settings.scanIntervalMs));
        ini.SetValue("Display", "OutlineColor", fmt::format("{:06X}", g_settings.outlineColor).c_str());
        ini.SetDoubleValue("Display", "GlowAlpha", g_settings.glowAlpha);
        ini.SetDoubleValue("Display", "MinOpacity", g_settings.minOpacity);
        ini.SetDoubleValue("Display", "OutlineThickness", g_settings.outlineThickness);
        ini.SetBoolValue("Display", "ShowOutline", g_settings.showOutline);
        ini.SetBoolValue("Display", "ShowGlow", g_settings.showGlow);
        ini.SetBoolValue("Display", "ShowCenterDot", g_settings.showCenterDot);
        ini.SetBoolValue("Display", "ShowIndicator", g_settings.showIndicator);
        ini.SetDoubleValue("Display", "FadeStartDistance", g_settings.fadeStartDistance);
        ini.SetDoubleValue("Display", "FadePower", g_settings.fadePower);

        SI_Error const saveRc = ini.SaveFile(path.string().c_str());
        if (saveRc < 0)
        {
            logger::warn("Failed to write INI at {}", path.string());
        }

        logger::info(
            "Config loaded: enabled={}, hotkey=0x{:02X}, maxDistance={:.0f}, scanInterval={}ms",
            g_settings.enabled,
            g_settings.hotkey,
            g_settings.maxDistance,
            g_settings.scanIntervalMs);
    }

    Settings const& Get() noexcept
    {
        return g_settings;
    }

    bool IsEnabled() noexcept
    {
        return g_enabled.load(std::memory_order_relaxed);
    }

    void SetEnabled(bool a_enabled) noexcept
    {
        g_settings.enabled = a_enabled;
        g_enabled.store(a_enabled, std::memory_order_relaxed);
    }

    void SaveEnabled() noexcept
    {
        CSimpleIniA ini;
        ini.SetUnicode();
        auto const path = GetIniPath();
        if (ini.LoadFile(path.string().c_str()) >= 0)
        {
            ini.SetBoolValue("General", "Enabled", IsEnabled());
            SI_Error const saveRc = ini.SaveFile(path.string().c_str());
            if (saveRc < 0)
            {
                logger::warn("Failed to save enabled state to INI at {}", path.string());
            }
        }
    }
}
