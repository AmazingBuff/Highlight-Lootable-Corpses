//
// Created by AmazingBuff on 2026/9/11.
//

#pragma once

constexpr std::string_view Skyrim_Plugin = "Skyrim.esm"sv;
constexpr std::string_view Dawnguard_Plugin = "Dawnguard.esm"sv;
constexpr std::string_view Dragonborn_Plugin = "Dragonborn.esm"sv;

struct LocalFromID
{
    uint32_t local_id;
    std::string_view plugin_name;
};

template<typename T, size_t N>
constexpr size_t array_size(const T(&)[N])
{
    return N;
}

template<typename T, size_t N>
    requires(std::is_integral_v<T>)
constexpr size_t hash_str(const T(&str)[N], const size_t& seed)
{
    size_t hash = seed;
    for (size_t i = 0; i < N - 1; ++i)
        hash = (hash ^ static_cast<size_t>(str[i])) * 16777619ull;

    return hash;
}

template<typename T, size_t N>
    requires(std::is_integral_v<T>)
constexpr size_t hash_str(const std::basic_string_view<T>& str, const size_t& seed)
{
    size_t hash = seed;
    for (size_t i = 0; i < N - 1; ++i)
        hash = (hash ^ static_cast<size_t>(str[i])) * 16777619ull;

    return hash;
}

template<typename T>
    requires(std::is_integral_v<T>)
constexpr size_t hash_str(const T* str, const size_t len, const size_t& seed)
{
    size_t hash = seed;
    for (size_t i = 0; i < len; ++i)
        hash = (hash ^ static_cast<size_t>(str[i])) * 16777619ull;

    return hash;
}

template<typename T, typename... Rest>
constexpr void hash_combine_mul(size_t& seed, const T& val, const Rest&... rest)
{
    if constexpr (std::is_convertible_v<T, size_t>)
        seed ^= (static_cast<size_t>(val) + 0x9e3779b9 + (seed << 6) + (seed >> 2));
    else
        seed ^= (std::hash<T>()(val) + 0x9e3779b9 + (seed << 6) + (seed >> 2));
    (hash_combine_mul(seed, rest), ...);
}

template<typename T>
constexpr size_t hash_combine(const size_t& seed, const T& val)
{
    if constexpr (std::is_convertible_v<T, size_t>)
        return seed ^ (static_cast<size_t>(val) + 0x9e3779b9 + (seed << 6) + (seed >> 2));
    else
        return seed ^ (std::hash<T>()(val) + 0x9e3779b9 + (seed << 6) + (seed >> 2));
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