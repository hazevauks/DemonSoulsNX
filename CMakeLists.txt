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
# ps3recomp's runtime, built for Horizon from the vendored sources as they are. src/hos stands
# in for the POSIX pieces Horizon does not have: shim headers searched first, a header forced
# into every source, its implementations, and mbedTLS's platform settings (docs/build.md).
# ---------------------------------------------------------------------------
option(DESNX_RUNTIME "Build ps3recomp's runtime library" ON)
if(DESNX_RUNTIME)
	add_subdirectory(third_party/ps3recomp EXCLUDE_FROM_ALL)
	target_include_directories(ps3recomp_runtime BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/src/hos/include)
	# cellPad and cellAudio include <SDL2/SDL.h>: portlibs' include folder itself
	target_include_directories(ps3recomp_runtime PRIVATE ${DEVKITPRO}/portlibs/switch/include)
	target_compile_options(ps3recomp_runtime PRIVATE -include ${CMAKE_SOURCE_DIR}/src/hos/hos_compat.h)
	target_compile_definitions(ps3recomp_runtime PRIVATE
		"MBEDTLS_USER_CONFIG_FILE=\"${CMAKE_SOURCE_DIR}/src/hos/hos_mbedtls_config.h\"")
	target_sources(ps3recomp_runtime PRIVATE ${CMAKE_SOURCE_DIR}/src/hos/hos_compat.c)
	add_custom_target(desnx_runtime ALL DEPENDS ps3recomp_runtime)
endif()

# ---------------------------------------------------------------------------
# The host side of the game's build: everything Soulbrandt's des_boot holds but the lifted code.
# ps3recomp's PPU boot scaffold, the HLE NID table (made from the runtime's own modules, not
# from the game) and Soulbrandt's game-specific code, with the flags Soulbrandt builds them with
# (its port/CMakeLists.txt). docs/build.md.
# ---------------------------------------------------------------------------
option(DESNX_HOST "Build the host side of the game (with DESNX_RUNTIME)" ON)
if(DESNX_RUNTIME AND DESNX_HOST)
	find_package(Python3 REQUIRED COMPONENTS Interpreter)
	set(PS3RECOMP ${CMAKE_SOURCE_DIR}/third_party/ps3recomp)
	set(SOULBRANDT_PORT ${CMAKE_SOURCE_DIR}/third_party/soulbrandt/port)

	add_custom_command(
		OUTPUT ${CMAKE_BINARY_DIR}/host/hle_nid_table.cpp
		COMMAND ${Python3_EXECUTABLE} -I ${PS3RECOMP}/tools/gen_hle_nids.py --all --out ${CMAKE_BINARY_DIR}/host/hle_nid_table.cpp
		DEPENDS ${PS3RECOMP}/tools/gen_hle_nids.py
		WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
		COMMENT "Generating the HLE NID table from the runtime's modules"
		VERBATIM
	)

	add_library(desnx_host STATIC
		${CMAKE_BINARY_DIR}/host/hle_nid_table.cpp
		${PS3RECOMP}/runtime/ppu/ppu_loader.cpp
		${PS3RECOMP}/runtime/ppu/ppu_hle.cpp
		${PS3RECOMP}/runtime/ppu/ppu_fs.cpp
		${PS3RECOMP}/runtime/ppu/ppu_sysprx.cpp
		${PS3RECOMP}/runtime/ppu/tests/boot_main.cpp
		${SOULBRANDT_PORT}/missing_stubs.cpp
		${SOULBRANDT_PORT}/des_heap.cpp
		${SOULBRANDT_PORT}/des_options.cpp
		${SOULBRANDT_PORT}/des_aspect.cpp
		${SOULBRANDT_PORT}/des_hud.cpp
		${SOULBRANDT_PORT}/des_menu_mouse.cpp
		${SOULBRANDT_PORT}/des_menu_sfx.cpp
		${SOULBRANDT_PORT}/des_chrins.cpp
		${SOULBRANDT_PORT}/des_souls_use.cpp
		${SOULBRANDT_PORT}/des_respec.cpp
		${SOULBRANDT_PORT}/des_prework.cpp
		${SOULBRANDT_PORT}/des_route.cpp
		${SOULBRANDT_PORT}/des_ui_stub.cpp      # no launcher UI: Soulbrandt's own build without it
		${CMAKE_SOURCE_DIR}/src/platform/nx_port_glue.c
	)
	set_target_properties(desnx_host PROPERTIES C_STANDARD 17 CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
	target_include_directories(desnx_host BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/src/hos/include)
	target_include_directories(desnx_host PRIVATE
		${PS3RECOMP}/include ${PS3RECOMP}/runtime/ppu ${PS3RECOMP}/libs/video
		${SOULBRANDT_PORT}/ui/include ${CMAKE_SOURCE_DIR}/src/platform
		${DEVKITPRO}/portlibs/switch/include)
	target_compile_options(desnx_host PRIVATE
		-include ${CMAKE_SOURCE_DIR}/src/hos/hos_compat.h
		-fwrapv -fno-strict-aliasing -ffp-contract=off
		-Wno-unused-parameter -Wno-unused-variable -Wno-unused-function -Wno-unused-label
		-Wno-unused-but-set-variable)
	# a lift without a shadow stack, and no developer cheats (Soulbrandt's defaults)
	target_compile_definitions(desnx_host PRIVATE _GNU_SOURCE DS_NO_SHADOW_STACK)
	set_property(SOURCE ${SOULBRANDT_PORT}/des_options.cpp ${SOULBRANDT_PORT}/des_menu_sfx.cpp
		APPEND PROPERTY COMPILE_DEFINITIONS DES_DEV=0)
	target_link_libraries(desnx_host PUBLIC ps3recomp_runtime)
endif()

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
