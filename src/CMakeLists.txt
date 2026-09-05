set(ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(SOURCE_DIR "${ROOT_DIR}/src")

# 小型稳定目标显式列出 sources，便于审查和可靠重新配置
set(SOURCE_FILES
	"${SOURCE_DIR}/config.cpp"
	"${SOURCE_DIR}/corpse_finder.cpp"
	"${SOURCE_DIR}/esp_renderer.cpp"
	"${SOURCE_DIR}/input.cpp"
	"${SOURCE_DIR}/loot_filter.cpp"
	"${SOURCE_DIR}/main.cpp"
	"${SOURCE_DIR}/shader_compile.cpp"
	"${SOURCE_DIR}/ui_menu.cpp"
)

set(HEADER_FILES
	"${SOURCE_DIR}/config.h"
	"${SOURCE_DIR}/corpse_finder.h"
	"${SOURCE_DIR}/esp_renderer.h"
	"${SOURCE_DIR}/input.h"
	"${SOURCE_DIR}/loot_filter.h"
	"${SOURCE_DIR}/pch.h"
	"${SOURCE_DIR}/shader_compile.h"
	"${SOURCE_DIR}/ui_menu.h"
)

set(VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/src/Plugin.h")
configure_file(
	"${ROOT_DIR}/cmake/Plugin.h.in"
	"${VERSION_HEADER}"
	@ONLY
)

configure_file(
	"${ROOT_DIR}/cmake/version.rc.in"
	"${CMAKE_CURRENT_BINARY_DIR}/version.rc"
	@ONLY
)

source_group("Source" FILES ${SOURCE_FILES})
source_group("Header" FILES ${HEADER_FILES} ${VERSION_HEADER})

add_library(
	${PROJECT_NAME}
	SHARED
	${HEADER_FILES}
	${SOURCE_FILES}
	${VERSION_HEADER}
	${CMAKE_CURRENT_BINARY_DIR}/version.rc
	${ROOT_DIR}/.clang-format
	${ROOT_DIR}/.editorconfig
)

target_compile_features(
	${PROJECT_NAME}
	PRIVATE
	cxx_std_23
)

# Align the MSVC toolset with the one vcpkg uses for dependencies (spdlog).
# CMake 3.31 drops the `version=` clause from CMAKE_GENERATOR_TOOLSET and writes
# a plain `v143`, which resolves to an older toolset (14.38) whose STL lacks the
# vectorized symbols 14.44 objects reference, causing LNK2019 at link time.
set_target_properties(
	${PROJECT_NAME}
	PROPERTIES
	VS_GLOBAL_VCToolsVersion "14.44.35207"
)

if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
	target_compile_options(
		${PROJECT_NAME}
		PRIVATE
		"/sdl"	# Enable Additional Security Checks
		"/utf-8"	# Set Source and Executable character sets to UTF-8
		"/Zi"	# Debug Information Format

		"/permissive-"	# Standards conformance
		"/Zc:preprocessor"	# Enable preprocessor conformance mode

		"/wd4200" # nonstandard extension used : zero-sized array in struct/union

		"$<$<CONFIG:DEBUG>:/ZI>"
		"$<$<CONFIG:RELEASE>:/Zi;/Zc:inline;/JMC-;/Ob3>"
	)

	target_compile_definitions(
		${PROJECT_NAME}
		PRIVATE
		NOMINMAX
	)

	target_link_options(
		${PROJECT_NAME}
		PRIVATE
		"$<$<CONFIG:DEBUG>:/INCREMENTAL;/OPT:NOREF;/OPT:NOICF>"
		"$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
	)
endif()

target_include_directories(
	${PROJECT_NAME}
	PRIVATE
	${CMAKE_CURRENT_BINARY_DIR}/src
	${SOURCE_DIR}
)

# dependency macros
macro(find_dependency_path DEPENDENCY FILE)
	# searches extern for dependencies and if not checks the environment variable
	if(NOT ${DEPENDENCY} STREQUAL "")
		# Check extern
		message(
			STATUS
			"Searching for ${DEPENDENCY} using file ${FILE}"
		)
		find_path(PATH
			${FILE}
			PATHS
			"../extern/${DEPENDENCY}"
			"extern/${DEPENDENCY}"
			"../external/${DEPENDENCY}"
			"external/${DEPENDENCY}")

		set("${DEPENDENCY}Path" "${PATH}")
		if("${${DEPENDENCY}Path}" STREQUAL "PATH-NOTFOUND")
			# Check path
			message(
				STATUS
				"Getting environment variable for ${DEPENDENCY}Path: $ENV{${DEPENDENCY}Path}"
			)
			set("${DEPENDENCY}Path" "$ENV{${DEPENDENCY}Path}")
		endif()

		if (NOT "${${DEPENDENCY}Path}" STREQUAL "")
			message(
				STATUS
				"Found ${DEPENDENCY} in ${${DEPENDENCY}Path}; adding"
			)
			add_subdirectory("${${DEPENDENCY}Path}" ${DEPENDENCY})
		endif()
	endif()
endmacro()

# dependencies
find_dependency_path(CommonLibSSE include/REL/Relocation.h)

if(("${CommonLibSSEPath}" STREQUAL "CommonLibSSEPath-NOTFOUND") OR "${CommonLibSSEPath}" STREQUAL "")
	message(FATAL_ERROR "CommonLibSSE not found; expected at extern/CommonLibSSE")
endif()

# CommonLibSSE 由本仓库源码构建时，SKSE_SUPPORT_PATCH_SAFETY=OFF 下
# Trampoline.cpp 有无用形参（C4100），与全局 /WX 冲突。在项目侧静默该警告，
# 保持三方库源码原样（不改 extern/CommonLibSSE）。
if(TARGET CommonLibSSE AND MSVC)
	target_compile_options(CommonLibSSE PRIVATE /wd4100)
endif()

# SKSE-MCP：header-only 的 SKSE Menu Framework 插件侧封装（动态加载 SKSEMenuFramework.dll）
add_subdirectory(
	"${ROOT_DIR}/extern/SKSE-MCP"
	"${CMAKE_CURRENT_BINARY_DIR}/SKSE-MCP"
)

find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(simpleini CONFIG REQUIRED)
find_package(directxtk CONFIG REQUIRED)

target_link_libraries(
	${PROJECT_NAME}
	PRIVATE
	spdlog::spdlog
	fmt::fmt
	CommonLibSSE::CommonLibSSE
	SimpleIni::SimpleIni
	Microsoft::DirectXTK
	SKSE-MCP::SKSE-MCP
	d3d11
	dxgi
	d3dcompiler
)

target_precompile_headers(
	${PROJECT_NAME}
	PRIVATE
	${SOURCE_DIR}/pch.h
)

install(
	FILES
		"$<TARGET_FILE:${PROJECT_NAME}>"
	DESTINATION "SKSE/Plugins"
	COMPONENT "main"
)

install(
	FILES
		"$<TARGET_PDB_FILE:${PROJECT_NAME}>"
	DESTINATION "/"
	COMPONENT "pdbs"
)

if("${COPY_OUTPUT}")
	add_custom_command(
		TARGET "${PROJECT_NAME}"
		POST_BUILD
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${CompiledPluginsPath}/SKSE/Plugins/"
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_PDB_FILE:${PROJECT_NAME}>" "${CompiledPluginsPath}/SKSE/Plugins/"
		VERBATIM
	)
endif()
