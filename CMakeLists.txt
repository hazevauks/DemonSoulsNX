cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{DEVKITPRO})
	set(CMAKE_TOOLCHAIN_FILE "$ENV{DEVKITPRO}/cmake/Switch.cmake")
endif()

project(DemonSoulsNX VERSION 0.0.1 LANGUAGES C CXX)

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
# Switch platform layer: logs, the console's memory, the game's folder
# ---------------------------------------------------------------------------
add_library(desnx_platform STATIC
	src/platform/nx_data.c
	src/platform/nx_log.c
	src/platform/nx_system.c
)
target_include_directories(desnx_platform PUBLIC src)

# ---------------------------------------------------------------------------
# The NRO. Stage 1: the first boot, with no game code yet (docs/status.md).
# ---------------------------------------------------------------------------
add_executable(demonsouls src/main.c)
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
