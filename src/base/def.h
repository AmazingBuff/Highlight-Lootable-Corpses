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

template<typename Tp, typename... Rest>
constexpr void hash_combine_mul(size_t& seed, const Tp& val, const Rest&... rest)
{
    if constexpr (std::is_convertible_v<Tp, size_t>)
        seed ^= (static_cast<size_t>(val) + 0x9e3779b9 + (seed << 6) + (seed >> 2));
    else
        seed ^= (std::hash<Tp>()(val) + 0x9e3779b9 + (seed << 6) + (seed >> 2));
    (hash_combine_mul(seed, rest), ...);
}

template<typename Tp>
constexpr size_t hash_combine(const size_t& seed, const Tp& val)
{
    if constexpr (std::is_convertible_v<Tp, size_t>)
        return seed ^ (static_cast<size_t>(val) + 0x9e3779b9 + (seed << 6) + (seed >> 2));
    else
        return seed ^ (std::hash<Tp>()(val) + 0x9e3779b9 + (seed << 6) + (seed >> 2));
}

inline size_t hash_combine(const size_t& seed, const void* mem, const size_t& length)
{
    uint8_t const* bytes = static_cast<uint8_t const*>(mem);
    size_t hash = seed;
    for (size_t i = 0; i < length; ++i)
        hash = (hash ^ static_cast<size_t>(bytes[i])) * 16777619ull;

    return hash;
}

static constexpr size_t Amazing_Hash = hash_str(Plugin::Plugin_Author.data(), Plugin::Plugin_Author.size(), 0);