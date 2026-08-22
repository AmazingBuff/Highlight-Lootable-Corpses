//
// Created by AmazingBuff on 2026/08/20.
//

#pragma once

#include <d3dcompiler.h>

namespace ShaderCompile
{
    // 编译一个内置 HLSL 源：失败时输出编译器诊断并返回 nullptr。
    // 返回的 blob 由调用方 Release（成对释放）。
    [[nodiscard]] ID3DBlob* compile(char const* a_source, char const* a_target, char const* a_name);
}
