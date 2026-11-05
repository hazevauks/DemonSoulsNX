cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{DEVKITPRO})
	set(CMAKE_TOOLCHAIN_FILE "$ENV{DEVKITPRO}/cmake/Switch.cmake")
endif()

project(DemonSoulsNX VERSION 0.0.1 LANGUAGES C CXX ASM)

if(NOT NINTENDO_SWITCH)
	message(FATAL_ERROR "DemonSoulsNX targets the Nintendo Switch: configure it with the devkitA64 toolchain.")
endif()

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

# frame pointers keep the crash reports readable
add_compile_options(-Wall -Wextra -fno-omit-frame-pointer)

# ---------------------------------------------------------------------------
# deko3D shaders: compiled by uam at build time, embedded in the NRO
# ---------------------------------------------------------------------------
nx_add_shader_program(triangle_vsh src/shaders/triangle_vsh.glsl vert)
nx_add_shader_program(triangle_fsh src/shaders/triangle_fsh.glsl frag)
dkp_add_embedded_binary_library(desnx_shaders triangle_vsh triangle_fsh)

# ---------------------------------------------------------------------------
# Switch platform layer: logs, memory, guest memory, files, the GPU and the cores
# ---------------------------------------------------------------------------
add_library(desnx_platform STATIC
	src/platform/nx_cpu_probe.c
	src/platform/nx_data.c
	src/platform/nx_gpu_probe.c
	src/platform/nx_guest.c
	src/platform/nx_log.c
	src/platform/nx_system.c
	src/platform/nx_vfs.c
	src/platform/nx_vfs_probe.c
)
target_include_directories(desnx_platform PUBLIC src)
# ps3recomp's header-only checks on game paths (ps3emu/vfs_path.h)
target_include_directories(desnx_platform PRIVATE third_party/ps3recomp/include)
find_package(ZLIB REQUIRED)    # switch-zlib, from portlibs
target_link_libraries(desnx_platform PUBLIC ZLIB::ZLIB deko3d PRIVATE desnx_shaders)

# ---------------------------------------------------------------------------
# The NRO. For now a test shell with no game code (docs/status.md).
# ---------------------------------------------------------------------------
# nx_guest_fault.s replaces libnx's exception entry, so it is linked as an object: from the
# library, the linker would take libnx's own, which the start code asks for first.
add_executable(demonsouls src/main.c src/platform/nx_guest_fault.s)
target_compile_definitions(demonsouls PRIVATE DESNX_VERSION="${PROJECT_VERSION}")
target_link_libraries(demonsouls PRIVATE desnx_platform nx m)
target_link_options(demonsouls PRIVATE -Wl,-Map=${CMAKE_BINARY_DIR}/demonsouls.map -Wl,--gc-sections)

nx_generate_nacp(demonsouls.nacp
	NAME "Demon's Souls"
	AUTHOR "hazevauks"
	VERSION "${PROJECT_VERSION}"
)
nx_create_nro(demonsouls
	NACP demonsouls.nacp
	ICON ${CMAKE_SOURCE_DIR}/assets/icon.jpg
)
