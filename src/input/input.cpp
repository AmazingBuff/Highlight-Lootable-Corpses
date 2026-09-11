#include "input.h"
#include "config/config.h"
#include "ui/ui_menu.h"

PLUGIN_NAMESPACE_BEGIN

namespace
{
    void button_event(RE::ButtonEvent* a_event)
    {
        if (Menu::is_menu_open())
            return;

        // just hotkey
        if (uint32_t const& vk = Setting::get_config().hotkey)
        {
            if (a_event->IsPressed() && a_event->GetIDCode() == vk)
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
        InputHandler(InputHandler&&) = delete;
        InputHandler(const InputHandler&) = delete;
        InputHandler& operator=(InputHandler&&) = delete;
        InputHandler& operator=(const InputHandler&) = delete;

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