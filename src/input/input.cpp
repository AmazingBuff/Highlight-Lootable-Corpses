#include "input.h"
#include "config/config.h"
#include "pulse_highlight.h"
#include "ui/ui_menu.h"

#include <Windows.h>

PLUGIN_NAMESPACE_BEGIN

namespace
{
    // INI 的 Hotkey 存 Windows 虚拟键码；SKSE 键盘事件的 idCode 是 DIK 扫描码，
    // 二者必须换算后比较——vk 与 DIK 仅部分区段数值相同（数字键/字母键恰好同值），
    // 直接比较是错误的。扩展键 vk（VK_LCONTROL 0xA2 等）没有对应 DIK，经
    // MapVirtualKey 归一到标准 DIK；映射失败（vk 无扫描码）时热键视为不可用。
    std::uint32_t dik_from_vk(std::uint32_t a_vk)
    {
        return a_vk > 0xFFu ? 0u : MapVirtualKeyA(a_vk, MAPVK_VK_TO_VSC);
    }

    // 把设备内裸码换算成 SKSE 宏键码（键盘 0-255 = DIK 扫描码、鼠标 256-263、
    // 手柄 266-281，偏移定义同 SKSE::InputMap）。鼠标/手柄键无统一 vk 约定，
    // INI 对这两类直接填宏键码（左键 256、右 257、中 258；手柄 266+）。
    // 注：idCode 直读成员而非 GetIDCode()——后者的多运行时重定位路径在 AE 上恒 0。
    bool macro_key_code(RE::ButtonEvent const& a_event, std::uint32_t& a_out)
    {
        switch (a_event.device.get())
        {
        case RE::INPUT_DEVICE::kKeyboard:
            a_out = a_event.idCode;
            return true;
        case RE::INPUT_DEVICE::kMouse:
            a_out = SKSE::InputMap::kMacro_MouseButtonOffset + a_event.idCode;
            return true;
        case RE::INPUT_DEVICE::kGamepad:
            a_out = SKSE::InputMap::kMacro_GamepadOffset + SKSE::InputMap::GamepadMaskToKeycode(a_event.idCode);
            return true;
        default:
            return false;
        }
    }

    void button_event(RE::ButtonEvent* a_event)
    {
        if (Menu::is_menu_open())
            return;

        std::uint32_t key = 0;
        if (!macro_key_code(*a_event, key))
            return;

        // just hotkey
        if (uint32_t const& vk = Setting::get_config().hotkey)
        {
            if (a_event->IsDown() && key == dik_from_vk(vk))
            {
                Config& cfg = Setting::get_config();
                // 消退模式：enable=true 时热键触发脉冲（渐隐途中重复按键重置进度，
                // 时长采用当前配置）；enable=false 时热键无效。
                if (cfg.hotkey_mode == Config::HotkeyMode::e_pulse)
                {
                    if (cfg.enabled)
                        PulseHighlight::trigger(cfg.pulse_duration_ms);
                }
                else
                {
                    bool& enabled = cfg.enabled;
                    enabled = !enabled;
                }
            }
        }
    }

    class InputHandler final : public RE::BSTEventSink<RE::InputEvent*>
    {
        InputHandler() = default;
        ~InputHandler() override = default;
    public:
        static InputHandler* get_singleton()
        {
            static InputHandler instance;
            return &instance;
        }

        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* event, RE::BSTEventSource<RE::InputEvent*>*) override
        {
            if (*event)
            {
                for (RE::InputEvent* current = *event; current; current = current->next)
                {
                    if (RE::ButtonEvent* button = current->AsButtonEvent())
                        button_event(button);
                }
            }

            return RE::BSEventNotifyControl::kContinue;
        }
    };
} // namespace

void InputManager::install()
{
    RE::BSInputDeviceManager::GetSingleton()->AddEventSink(InputHandler::get_singleton());

    logger::info("Installed input handler!");
}

PLUGIN_NAMESPACE_END
