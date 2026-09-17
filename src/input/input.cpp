#include "input.h"

#include "config/config.h"
#include "ui/pulse_timer.h"
#include "ui/ui_menu.h"

#include "Plugin.h"

PLUGIN_NAMESPACE_BEGIN

namespace
{
    uint32_t dik_from_vk(uint32_t vk)
    {
        return vk > 0xFFu ? 0u : MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    }

    bool macro_key_code(RE::ButtonEvent const& event, uint32_t& out)
    {
        switch (event.device.get())
        {
        case RE::INPUT_DEVICE::kKeyboard:
            out = event.idCode;
            return true;
        case RE::INPUT_DEVICE::kMouse:
            out = SKSE::InputMap::kMacro_MouseButtonOffset + event.idCode;
            return true;
        case RE::INPUT_DEVICE::kGamepad:
            out = SKSE::InputMap::kMacro_GamepadOffset + SKSE::InputMap::GamepadMaskToKeycode(event.idCode);
            return true;
        default:
            return false;
        }
    }

    void button_event(RE::ButtonEvent* event)
    {
        if (Menu::is_menu_open())
            return;

        uint32_t key = 0;
        if (!macro_key_code(*event, key))
            return;

        // just hotkey
        if (uint32_t const& vk = Setting::instance().get_config().hotkey)
        {
            if (event->IsDown() && key == dik_from_vk(vk))
            {
                Config& cfg = Setting::instance().get_config();
                if (cfg.hotkey_mode == Config::HotkeyMode::e_pulse)
                {
                    if (cfg.enabled)
                        PulseTimer::instance().trigger(cfg.pulse_duration_ms);
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
        static InputHandler* instance()
        {
            static InputHandler s_instance;
            return &s_instance;
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
    RE::BSInputDeviceManager::GetSingleton()->AddEventSink(InputHandler::instance());

    logger::info("Installed input handler!");
}

PLUGIN_NAMESPACE_END
