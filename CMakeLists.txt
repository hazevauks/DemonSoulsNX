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

# libnx, deko3D, SDL2 and zlib: desnx::libraries
include(cmake/desnx_libraries.cmake)

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
	src/platform/nx_crash.c
	src/platform/nx_data.c
	src/platform/nx_gpu_probe.c
	src/platform/nx_guest.c
	src/platform/nx_log.c
	src/platform/nx_sdl_probe.c
	src/platform/nx_settings.c
	src/platform/nx_system.c
	src/platform/nx_vfs.c
	src/platform/nx_vfs_probe.c
)
target_include_directories(desnx_platform PUBLIC src)
# ps3recomp's header-only checks on game paths (ps3emu/vfs_path.h)
target_include_directories(desnx_platform PRIVATE third_party/ps3recomp/include)
# The libraries link into whatever uses the platform layer, but their headers stay its own: the
# lifted code keeps the include folders it compiles with (zlib's portlibs folder only), or every
# chunk would compile again, for hours
target_link_libraries(desnx_platform
	PUBLIC ZLIB::ZLIB
	PRIVATE desnx::libraries desnx_shaders
	INTERFACE $<LINK_ONLY:desnx::libraries>)

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
	# The portable renderer (libs/video/rsxcore on libs/video/gpu) and its route switch, rsx_out (stage 12).
	# Defined here, PRIVATE to the runtime and the host side: ps3recomp's own PS3RECOMP_RSX_HAL option makes it
	# PUBLIC, which would put it on every lifted chunk's command line and compile them all again, for hours
	target_compile_definitions(ps3recomp_runtime PRIVATE PS3RECOMP_RSX_HAL=1)
	target_include_directories(ps3recomp_runtime PRIVATE ${CMAKE_SOURCE_DIR}/third_party/ps3recomp/libs/video/gpu)
	# The compile service's spv target, for the game's shaders on the console (stage 12c): glslang and
	# SPIRV-Tools, fetched and built here (cmake/desnx_shader_compilers.cmake), PRIVATE as the rest
	include(cmake/desnx_shader_compilers.cmake)
	target_sources(ps3recomp_runtime PRIVATE
		${CMAKE_SOURCE_DIR}/third_party/ps3recomp/libs/video/gpu/gpu_shader_spv.cpp
		${CMAKE_SOURCE_DIR}/third_party/ps3recomp/libs/video/gpu/gpu_shader_spv_edit.cpp)
	target_compile_definitions(ps3recomp_runtime PRIVATE PS3RECOMP_RSX_HAL_SPV=1)
	target_link_libraries(ps3recomp_runtime PRIVATE
		glslang glslang-default-resource-limits SPIRV-Tools-opt SPIRV-Tools-static)
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

	# Objects, not an archive: Soulbrandt compiles these into des_boot itself, and from an archive
	# the linker would leave out what nothing asks for by name. ppu_hle.cpp has an empty weak
	# ppu_hle_register_all, so the NID table's real one would never be taken, and missing_stubs.cpp
	# sets its hooks from a static constructor.
	add_library(desnx_host OBJECT
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
		${CMAKE_SOURCE_DIR}/src/platform/nx_game_main.c   # the Switch's start, before the boot's main
		${CMAKE_SOURCE_DIR}/src/platform/nx_game_sdl.c    # the game's sound and controls through SDL2
		${CMAKE_SOURCE_DIR}/src/platform/nx_game_threads.c   # detached threads, which libnx lacks
		${CMAKE_SOURCE_DIR}/src/platform/nx_game_io.c        # the game's file I/O, counted
		${CMAKE_SOURCE_DIR}/src/platform/nx_cache_mem.c      # the game's cache (/dev_hdd1) in memory
	)
	set_property(SOURCE ${CMAKE_SOURCE_DIR}/src/platform/nx_game_main.c
		APPEND PROPERTY COMPILE_DEFINITIONS DESNX_VERSION="${PROJECT_VERSION}")
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
	# a lift without a shadow stack, and no developer cheats (Soulbrandt's defaults); the boot and the port reach
	# the renderer through the portable renderer's route switch (PS3RECOMP_RSX_HAL, as for the runtime above)
	target_compile_definitions(desnx_host PRIVATE _GNU_SOURCE DS_NO_SHADOW_STACK PS3RECOMP_RSX_HAL=1)
	target_include_directories(desnx_host PRIVATE ${PS3RECOMP}/libs/video/gpu)
	set_property(SOURCE ${SOULBRANDT_PORT}/des_options.cpp ${SOULBRANDT_PORT}/des_menu_sfx.cpp
		APPEND PROPERTY COMPILE_DEFINITIONS DES_DEV=0)
	target_link_libraries(desnx_host PUBLIC ps3recomp_runtime)

	# The deko3D backend of the portable renderer and the core compiled for it (stage 12,
	# docs/deko3d.md), with the built-in passes' shaders uam compiles here and the NRO embeds
	nx_add_shader_program(blit_vsh src/gpu/shaders/blit_vsh.glsl vert)
	nx_add_shader_program(blit_fsh src/gpu/shaders/blit_fsh.glsl frag)
	dkp_add_embedded_binary_library(desnx_gpu_shaders blit_vsh blit_fsh)
	# with the game's shaders compiled on the console: SPIR-V to GLSL (SPIRV-Cross) to DKSH (uam)
	add_subdirectory(third_party/uam EXCLUDE_FROM_ALL)
	add_library(desnx_gpu OBJECT src/gpu/gpu_dk.c src/gpu/rsxc_dk.c src/gpu/gpu_dk_sc.cpp)
	set_target_properties(desnx_gpu PROPERTIES C_STANDARD 17 CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
	target_include_directories(desnx_gpu BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/src/hos/include)
	target_include_directories(desnx_gpu PRIVATE
		${CMAKE_SOURCE_DIR}/src/gpu ${PS3RECOMP}/include ${PS3RECOMP}/libs/video ${PS3RECOMP}/libs/video/gpu
		${PS3RECOMP}/libs/video/rsxcore ${DEVKITPRO}/portlibs/switch/include)
	target_compile_options(desnx_gpu PRIVATE
		-include ${CMAKE_SOURCE_DIR}/src/hos/hos_compat.h
		-fno-strict-aliasing -ffp-contract=off
		-Wno-unused-parameter -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable)
	target_compile_definitions(desnx_gpu PRIVATE _GNU_SOURCE PS3RECOMP_RSX_HAL=1)
	# the shaders' headers for these two files only; their objects go into the game itself (below), as
	# an object library's own objects reach only a target that lists them
	target_link_libraries(desnx_gpu PUBLIC ps3recomp_runtime
		PRIVATE desnx_gpu_shaders spirv-cross-glsl spirv-cross-core uam_lib)
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

# ---------------------------------------------------------------------------
# The game: the lifted code, made on this PC from your EBOOT (docs/build.md), with the host side
# and the runtime. Defined only when work/recomp holds a lift, so CI never has it. What it makes
# holds code from your game: it never goes in the repository or a release.
# ---------------------------------------------------------------------------
set(DESNX_LIFT_DIR ${CMAKE_SOURCE_DIR}/work CACHE PATH "the lift's folder")
file(GLOB DESNX_PPU_CHUNKS CONFIGURE_DEPENDS ${DESNX_LIFT_DIR}/recomp/ppu_recomp_*.cpp)
if(TARGET desnx_host AND DESNX_PPU_CHUNKS)
	file(GLOB DESNX_SPU_CHUNKS CONFIGURE_DEPENDS ${DESNX_LIFT_DIR}/spu/*/spu_recomp.c)
	set(DESNX_GAME_SOURCES ${DESNX_PPU_CHUNKS} ${DESNX_SPU_CHUNKS})
	if(EXISTS ${DESNX_LIFT_DIR}/spu/_gen/spu_workloads.cpp)
		list(APPEND DESNX_GAME_SOURCES ${DESNX_LIFT_DIR}/spu/_gen/spu_workloads.cpp)
	endif()
	# libsre's SPURS kernel, lifted from your firmware's copy: the lifter writes C++ into a .c file
	set(DESNX_LIBSRE_LIFTED ${DESNX_LIFT_DIR}/libsre/lifted/ppu_recomp.c)
	set(DESNX_LIBSRE_GLUE ${DESNX_LIFT_DIR}/libsre/libsre_glue.c)
	if(EXISTS ${DESNX_LIBSRE_LIFTED} AND EXISTS ${DESNX_LIBSRE_GLUE})
		set_source_files_properties(${DESNX_LIBSRE_LIFTED} PROPERTIES LANGUAGE CXX)
		set_source_files_properties(${DESNX_LIBSRE_GLUE} PROPERTIES INCLUDE_DIRECTORIES ${PS3RECOMP}/runtime/prx)
		list(APPEND DESNX_GAME_SOURCES ${DESNX_LIBSRE_LIFTED} ${DESNX_LIBSRE_GLUE} ${PS3RECOMP}/runtime/prx/prx_loader.c)
	endif()
	list(LENGTH DESNX_PPU_CHUNKS DESNX_N_PPU)
	list(LENGTH DESNX_SPU_CHUNKS DESNX_N_SPU)
	message(STATUS "Game: ${DESNX_N_PPU} lifted PPU chunks, ${DESNX_N_SPU} lifted SPU images")

	add_executable(demonsouls_game ${DESNX_GAME_SOURCES} src/platform/nx_guest_fault.s
		$<TARGET_OBJECTS:desnx_gpu_shaders> ${UAM_OBJECT})
	set_source_files_properties(${UAM_OBJECT} PROPERTIES EXTERNAL_OBJECT ON GENERATED ON)
	add_dependencies(demonsouls_game uam_link)
	set_target_properties(demonsouls_game PROPERTIES C_STANDARD 17 CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
	target_include_directories(demonsouls_game BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/src/hos/include)
	target_include_directories(demonsouls_game PRIVATE
		${DESNX_LIFT_DIR}/recomp ${PS3RECOMP}/include ${PS3RECOMP}/runtime/ppu ${PS3RECOMP}/runtime/spu
		${PS3RECOMP}/libs/video)
	# Soulbrandt's flags for the lifted code (its port/CMakeLists.txt), without its x86 SSE4.1:
	# ps3emu/ps3_sse.h gives the same operations on ARM. No debug information (-g0 over the -g
	# devkitPro's Release flags carry): the code is generated, crash reports name its functions from
	# the symbol table, and GCC's variable tracking takes hours on the longest lifted functions
	target_compile_options(demonsouls_game PRIVATE
		$<$<COMPILE_LANGUAGE:C,CXX>:-include ${CMAKE_SOURCE_DIR}/src/hos/hos_compat.h>
		-O2 -g0 -fwrapv -fno-strict-aliasing -ffp-contract=off
		-Wno-unused-parameter -Wno-unused-variable -Wno-unused-function -Wno-unused-label
		-Wno-unused-but-set-variable)
	target_compile_definitions(demonsouls_game PRIVATE _GNU_SOURCE DS_NO_SHADOW_STACK)
	target_link_libraries(demonsouls_game PRIVATE desnx_host desnx_gpu desnx_platform nx m)
	# libnx's start code calls nx_game_main.c's __wrap_main, which sets up the Switch and then runs
	# the boot's main; and host stacks are capped at 16 MB, as libnx takes a whole stack from the heap.
	# nx_game_sdl.c: cellAudio's device buffer and cellPad's controllers, as the Switch's SDL2 needs
	target_link_options(demonsouls_game PRIVATE -Wl,-Map=${CMAKE_BINARY_DIR}/demonsouls_game.map -Wl,--gc-sections
		-Wl,--wrap=main -Wl,--wrap=pthread_attr_setstacksize -Wl,--wrap=setvbuf -Wl,--wrap=pthread_create
		-Wl,--wrap=abort -Wl,--wrap=__assert_func
		-Wl,--wrap=_open_r -Wl,--wrap=_read_r -Wl,--wrap=_write_r -Wl,--wrap=_close_r
		-Wl,--wrap=SDL_OpenAudioDevice -Wl,--wrap=SDL_QueueAudio -Wl,--wrap=SDL_SetThreadPriority
		-Wl,--wrap=SDL_IsGameController -Wl,--wrap=SDL_GameControllerOpen -Wl,--wrap=SDL_GameControllerGetAttached
		-Wl,--wrap=SDL_GameControllerGetButton -Wl,--wrap=SDL_GameControllerGetAxis)
	# Hours to compile on a laptop, so only on request: cmake --build build --target demonsouls_game_nro
	# (nx_create_nro would put it in every build).
	set_target_properties(demonsouls_game PROPERTIES EXCLUDE_FROM_ALL ON)
	# the version tells it from the test NRO in the homebrew menu, beside it in switch/demonsouls
	nx_generate_nacp(demonsouls_game.nacp
		NAME "Demon's Souls"
		AUTHOR "hazevauks"
		VERSION "${PROJECT_VERSION} game"
	)
	add_custom_command(
		OUTPUT ${CMAKE_BINARY_DIR}/demonsouls_game.nro
		COMMAND ${NX_ELF2NRO_EXE} $<TARGET_FILE:demonsouls_game> ${CMAKE_BINARY_DIR}/demonsouls_game.nro
			--icon=${CMAKE_SOURCE_DIR}/assets/icon.jpg --nacp=${CMAKE_BINARY_DIR}/demonsouls_game.nacp
		DEPENDS demonsouls_game ${CMAKE_SOURCE_DIR}/assets/icon.jpg ${CMAKE_BINARY_DIR}/demonsouls_game.nacp
		COMMENT "Building the game's NRO"
		VERBATIM
	)
	add_custom_target(demonsouls_game_nro DEPENDS ${CMAKE_BINARY_DIR}/demonsouls_game.nro)
endif()
