#pragma once

#include "Plugin.h"

PLUGIN_NAMESPACE_BEGIN

class InputManager
{
public:
    InputManager() = delete;
    ~InputManager() = delete;
    InputManager(InputManager const&) = delete;
    InputManager(InputManager const&&) = delete;
    InputManager operator=(InputManager&) = delete;
    InputManager operator=(InputManager&&) = delete;

    static void install();
};

PLUGIN_NAMESPACE_END
