#pragma once

#pragma warning(push)
#include <RE/Skyrim.h>
#include <REL/Relocation.h>
#include <SKSE/SKSE.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#include <fmt/format.h>
#pragma warning(pop)

// Direct3D / DirectXTK
#pragma warning(push)
#pragma warning(disable: 4324)  // structure was padded due to alignment specifier
#include <d3d11.h>
#include <dxgi.h>
#include <DirectXMath.h>
#include <CommonStates.h>
#include <Effects.h>
#include <PrimitiveBatch.h>
#include <VertexTypes.h>
#pragma warning(pop)

#include <SimpleIni.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>

using namespace std::literals;

namespace logger = SKSE::log;

namespace util
{
    using SKSE::stl::report_and_fail;
}

#define DLLEXPORT __declspec(dllexport)

#include "Plugin.h"
