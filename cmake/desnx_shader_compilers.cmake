# The shader compilers the game runs on the console (stage 12c, docs/deko3d.md): the core's HLSL
# to SPIR-V through glslang (ps3recomp's spv target, libs/video/gpu/gpu_shader_spv*.cpp), SPIR-V
# to GLSL through SPIRV-Cross, and GLSL to DKSH through uam (third_party/uam).
#
# glslang, SPIRV-Tools, SPIRV-Headers and SPIRV-Cross are fetched at the pins ps3recomp's own
# cmake/gpu_deps.cmake names (Vulkan SDK 1.4.363.0), built static for Horizon, libraries only.
# Their builds run Python 3 (SPIR-V's grammar tables, glslang's build_info.h). A build without a
# network names its copies with FetchContent's variables:
#   -DFETCHCONTENT_SOURCE_DIR_SPIRV_HEADERS=<dir>  -DFETCHCONTENT_SOURCE_DIR_SPIRV_TOOLS=<dir>
#   -DFETCHCONTENT_SOURCE_DIR_GLSLANG=<dir>        -DFETCHCONTENT_SOURCE_DIR_SPIRV_CROSS=<dir>

include(FetchContent)
FetchContent_Declare(spirv_headers
	GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers.git
	GIT_TAG 496543121ce6419f23d6fa5d7194ba66c36212d2   # vulkan-sdk-1.4.363.0
	EXCLUDE_FROM_ALL)
FetchContent_Declare(spirv_tools
	GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Tools.git
	GIT_TAG ef96ed763b43b59b33b31b362f09a02b729fa1c9   # vulkan-sdk-1.4.363.0
	EXCLUDE_FROM_ALL)
FetchContent_Declare(glslang
	GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
	GIT_TAG e1b562a8bed273a02f30b59b66a5d499793cede5   # vulkan-sdk-1.4.363.0
	EXCLUDE_FROM_ALL)
FetchContent_Declare(spirv_cross
	GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
	GIT_TAG f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7   # vulkan-sdk-1.4.363.0
	EXCLUDE_FROM_ALL)

# their option()s take the values set here (SPIRV-Cross's CMakeLists asks for no policy itself);
# no precompiled headers: glslang's names its header by a path the devkitPro shell may give in its
# own form (/home/...), which the Windows compiler cannot open
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)
set(_desnx_shared "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)
set(SPIRV_HEADERS_ENABLE_TESTS OFF)
set(SPIRV_HEADERS_ENABLE_INSTALL OFF)
FetchContent_MakeAvailable(spirv_headers)
set(SPIRV-Headers_SOURCE_DIR "${spirv_headers_SOURCE_DIR}")
set(SPIRV_SKIP_TESTS ON)
set(SPIRV_SKIP_EXECUTABLES ON)
set(SKIP_SPIRV_TOOLS_INSTALL ON)
set(SPIRV_WERROR OFF)
set(SPIRV_BUILD_FUZZER OFF)
set(SPIRV_TOOLS_BUILD_STATIC ON)
set(SPIRV_TOOLS_USE_MIMALLOC OFF)
FetchContent_MakeAvailable(spirv_tools)
set(GLSLANG_TESTS OFF)
set(GLSLANG_ENABLE_INSTALL OFF)
set(ENABLE_GLSLANG_BINARIES OFF)
set(ENABLE_GLSLANG_JS OFF)
set(ENABLE_HLSL ON)
set(ENABLE_OPT ON)
set(BUILD_EXTERNAL OFF)
FetchContent_MakeAvailable(glslang)
set(SPIRV_CROSS_SHARED OFF)
set(SPIRV_CROSS_STATIC ON)
set(SPIRV_CROSS_CLI OFF)
set(SPIRV_CROSS_ENABLE_TESTS OFF)
set(SPIRV_CROSS_ENABLE_GLSL ON)
set(SPIRV_CROSS_ENABLE_HLSL OFF)
set(SPIRV_CROSS_ENABLE_MSL OFF)
set(SPIRV_CROSS_ENABLE_CPP OFF)
set(SPIRV_CROSS_ENABLE_REFLECT OFF)
set(SPIRV_CROSS_ENABLE_C_API OFF)
set(SPIRV_CROSS_ENABLE_UTIL OFF)
set(SPIRV_CROSS_SKIP_INSTALL ON)
FetchContent_MakeAvailable(spirv_cross)
set(BUILD_SHARED_LIBS "${_desnx_shared}")
message(STATUS "Shaders on the console: glslang, SPIRV-Tools, SPIRV-Cross (fetched at their pins) and uam")
