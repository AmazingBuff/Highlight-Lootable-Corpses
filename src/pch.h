#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <RE/Skyrim.h>
#include <REL/Relocation.h>
#include <SKSE/SKSE.h>
#include <fmt/format.h>


using namespace std::literals;

namespace logger = SKSE::log;

static constexpr uint32_t Max_Corpse_Count = 16;

#define DLLEXPORT __declspec(dllexport)

#include "Plugin.h"