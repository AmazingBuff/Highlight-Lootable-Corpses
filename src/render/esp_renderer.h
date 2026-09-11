#pragma once

#include <d3d11.h>
#include <dxgi.h>

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
