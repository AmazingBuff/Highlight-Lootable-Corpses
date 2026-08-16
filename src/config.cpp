#include "pch.h"
#include "config.h"

namespace
{
    Config::Settings g_settings;
    std::atomic<bool> g_enabled{ true };
}

namespace Config
{
    std::filesystem::path get_ini_path() noexcept
    {
        // 放在游戏 Data\SKSE\Plugins\ 下，与 DLL 同目录
        auto const exePath = REL::Module::get().filePath();  // SkyrimSE.exe 的完整路径（wstring_view）
        std::filesystem::path game_root(exePath);
        return game_root.parent_path() / "Data" / "SKSE" / "Plugins" / (std::string(Plugin::NAME) + ".ini");
    }

    namespace
    {
        std::uint32_t parse_hex(char const* a_value, std::uint32_t a_default) noexcept
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

    void load() noexcept
    {
        CSimpleIniA ini;
        ini.SetUnicode();

        auto const path = get_ini_path();
        SI_Error const rc = ini.LoadFile(path.string().c_str());
        if (rc < 0)
        {
            logger::info("INI not found at {}, writing defaults", path.string());
        }

        g_settings.enabled = ini.GetBoolValue("General", "Enabled", g_settings.enabled);
        g_settings.hotkey = static_cast<std::uint32_t>(ini.GetLongValue("General", "Hotkey", static_cast<long>(g_settings.hotkey)));
        g_settings.max_distance = static_cast<float>(ini.GetDoubleValue("General", "MaxDistance", g_settings.max_distance));
        g_settings.scan_interval_ms = static_cast<std::uint32_t>(ini.GetLongValue("General", "ScanIntervalMs", static_cast<long>(g_settings.scan_interval_ms)));

        g_settings.outline_color = parse_hex(ini.GetValue("Display", "OutlineColor", "00FF66"), 0x00FF66);
        g_settings.min_opacity = static_cast<float>(ini.GetDoubleValue("Display", "MinOpacity", g_settings.min_opacity));
        g_settings.outline_thickness = static_cast<float>(ini.GetDoubleValue("Display", "OutlineThickness", g_settings.outline_thickness));
        g_settings.show_outline = ini.GetBoolValue("Display", "ShowOutline", g_settings.show_outline);
        g_settings.fade_start_distance = static_cast<float>(ini.GetDoubleValue("Display", "FadeStartDistance", g_settings.fade_start_distance));
        g_settings.fade_power = static_cast<float>(ini.GetDoubleValue("Display", "FadePower", g_settings.fade_power));

        g_enabled.store(g_settings.enabled, std::memory_order_relaxed);

        // 写回，保证文件存在且包含全部选项说明
        save();

        logger::info(
            "Config loaded: enabled={}, hotkey=0x{:02X}, max_distance={:.0f}, scanInterval={}ms",
            g_settings.enabled,
            g_settings.hotkey,
            g_settings.max_distance,
            g_settings.scan_interval_ms);
    }

    void save() noexcept
    {
        CSimpleIniA ini;
        ini.SetUnicode();
        auto const path = get_ini_path();
        if (ini.LoadFile(path.string().c_str()) < 0)
        {
            logger::info("INI not found at {}, writing defaults", path.string());
        }

        ini.SetBoolValue("General", "Enabled", g_settings.enabled);
        ini.SetLongValue("General", "Hotkey", static_cast<long>(g_settings.hotkey));
        ini.SetDoubleValue("General", "MaxDistance", g_settings.max_distance);
        ini.SetLongValue("General", "ScanIntervalMs", static_cast<long>(g_settings.scan_interval_ms));
        ini.SetValue("Display", "OutlineColor", fmt::format("{:06X}", g_settings.outline_color).c_str());
        ini.SetDoubleValue("Display", "MinOpacity", g_settings.min_opacity);
        ini.SetDoubleValue("Display", "OutlineThickness", g_settings.outline_thickness);
        ini.SetBoolValue("Display", "ShowOutline", g_settings.show_outline);
        ini.SetDoubleValue("Display", "FadeStartDistance", g_settings.fade_start_distance);
        ini.SetDoubleValue("Display", "FadePower", g_settings.fade_power);

        SI_Error const save_rc = ini.SaveFile(path.string().c_str());
        if (save_rc < 0)
        {
            logger::warn("Failed to write INI at {}", path.string());
        }
    }

    void reset_defaults() noexcept
    {
        g_settings = Settings{};
        g_enabled.store(g_settings.enabled, std::memory_order_relaxed);
        save();
        logger::info("Config reset to defaults");
    }

    Settings const& get() noexcept
    {
        return g_settings;
    }

    Settings& get_mutable() noexcept
    {
        return g_settings;
    }

    bool is_enabled() noexcept
    {
        return g_enabled.load(std::memory_order_relaxed);
    }

    void set_enabled(bool a_enabled) noexcept
    {
        g_settings.enabled = a_enabled;
        g_enabled.store(a_enabled, std::memory_order_relaxed);
    }

    void save_enabled() noexcept
    {
        CSimpleIniA ini;
        ini.SetUnicode();
        auto const path = get_ini_path();
        if (ini.LoadFile(path.string().c_str()) >= 0)
        {
            ini.SetBoolValue("General", "Enabled", is_enabled());
            SI_Error const save_rc = ini.SaveFile(path.string().c_str());
            if (save_rc < 0)
            {
                logger::warn("Failed to save enabled state to INI at {}", path.string());
            }
        }
    }
}
