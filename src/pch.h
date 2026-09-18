#pragma once

#include <RE/Skyrim.h>
#include <REL/Relocation.h>
#include <SKSE/SKSE.h>
#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <REX/W32/D3D11.h>

using namespace std::literals;

namespace logger = SKSE::log;

static constexpr uint32_t Max_Corpse_Count = 16;

#define DLLEXPORT __declspec(dllexport)

#include "Plugin.h"