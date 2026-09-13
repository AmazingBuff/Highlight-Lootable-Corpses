#include "input.h"
#include "config/config.h"
#include "ui/ui_menu.h"

#include <Windows.h>

PLUGIN_NAMESPACE_BEGIN

namespace
{
    std::uint32_t dik_from_vk(std::uint32_t a_vk)
    {
        return a_vk > 0xFFu ? 0u : MapVirtualKeyA(a_vk, MAPVK_VK_TO_VSC);
    }

    void button_event(RE::ButtonEvent* a_event)
    {
        if (Menu::is_menu_open())
            return;

        // just hotkey
        if (uint32_t const& vk = Setting::get_config().hotkey)
        {
            if (a_event->IsDown() && a_event->GetIDCode() == dik_from_vk(vk))
            {
                bool& enabled = Setting::get_config().enabled;
                enabled = !enabled;
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
