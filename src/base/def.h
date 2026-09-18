//
// Created by AmazingBuff on 2026/9/11.
//

#pragma once

template<typename T>
    requires(std::is_integral_v<T>)
constexpr size_t hash_str(T const* str, size_t const len, size_t const& seed)
{
    size_t hash = seed;
    for (size_t i = 0; i < len; ++i)
        hash = (hash ^ static_cast<size_t>(str[i])) * 16777619ull;

    return hash;
}

static constexpr size_t Amazing_Hash = hash_str(Plugin::Plugin_Author.data(), Plugin::Plugin_Author.size(), 0);