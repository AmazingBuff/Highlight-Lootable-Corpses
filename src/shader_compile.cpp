//
// Created by AmazingBuff on 2026/08/20.
//

#include "pch.h"
#include "shader_compile.h"

namespace ShaderCompile
{
    ID3DBlob* compile(char const* a_source, char const* a_target, char const* a_name)
    {
        if (!a_source || !a_target)
            return nullptr;

        ID3DBlob* blob = nullptr;
        ID3DBlob* err = nullptr;
        HRESULT const hr = D3DCompile(a_source, std::strlen(a_source), nullptr, nullptr, nullptr, "main", a_target, 0, 0, &blob, &err);
        if (FAILED(hr))
        {
            logger::error(
                "Shader compile failed [{} {}] ({:X}): {}",
                a_name ? a_name : "?",
                a_target,
                static_cast<unsigned int>(hr),
                err ? static_cast<char const*>(err->GetBufferPointer()) : "no diagnostics");
            if (blob)
            {
                blob->Release();
                blob = nullptr;
            }
        }
        if (err)
            err->Release();

        return blob;
    }
}
