//
// Created by AmazingBuff on 2026/9/13.
//

#pragma once

#include "Plugin.h"

PLUGIN_NAMESPACE_BEGIN

class Renderer
{
public:
    Renderer() = delete;
    ~Renderer() = delete;
    Renderer(Renderer const&) = delete;
    Renderer(Renderer const&&) = delete;
    Renderer operator=(Renderer&) = delete;
    Renderer operator=(Renderer&&) = delete;

    static void install();
};

PLUGIN_NAMESPACE_END
