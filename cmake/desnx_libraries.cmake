# desnx_libraries.cmake -- the libraries the port links against, in one place (docs/build.md).
#
# devkitPro's toolchain file ($DEVKITPRO/cmake/Switch.cmake) sets up devkitA64 and libnx; the
# rest are devkitPro's packages too: deko3D beside libnx, SDL2 (with the Mesa it is built with)
# and zlib in portlibs. desnx::libraries carries them all, in the order they link, for the test
# NRO and the game. ps3recomp's runtime finds the same SDL2 by itself (its CMakeLists.txt).
#
# Each package's version is read from devkitPro's package database and checked against the one
# the port was last tested with on a console: an older one is a warning, as it may lack what the
# port uses; a newer one is reported, as it has not been on a console yet.

set(DESNX_TESTED_PACKAGES
	"devkitA64 r30"
	"libnx 4.12.0"
	"deko3d 0.5.0"
	"uam 1.1.0"
	"switch-tools 1.13.1"
	"switch-sdl2 2.28.5"
	"switch-mesa 20.1.0"
	"switch-zlib 1.3.1"
)

find_library(DESNX_LIBNX nx REQUIRED)
find_library(DESNX_DEKO3D deko3d REQUIRED)
find_path(DESNX_DEKO3D_INCLUDE deko3d.h REQUIRED)
find_package(ZLIB REQUIRED)
find_package(SDL2 2.28 REQUIRED CONFIG)

add_library(desnx_libraries INTERFACE)
add_library(desnx::libraries ALIAS desnx_libraries)
target_include_directories(desnx_libraries INTERFACE ${DESNX_DEKO3D_INCLUDE})
# SDL2::SDL2 brings Mesa's EGL and libnx after it; deko3D, zlib and libnx itself follow
target_link_libraries(desnx_libraries INTERFACE SDL2::SDL2 ${DESNX_DEKO3D} ZLIB::ZLIB ${DESNX_LIBNX} m)

# devkitPro's package database: the MSYS2 one on Windows, dkp-pacman's on Linux
set(_desnx_db "")
foreach(_dir "${DEVKITPRO}/msys2/var/lib/pacman/local" "/var/lib/pacman/local"
             "${DEVKITPRO}/pacman/var/lib/pacman/local" "/opt/devkitpro/pacman/var/lib/pacman/local")
	if(IS_DIRECTORY "${_dir}" AND NOT _desnx_db)
		file(GLOB _found LIST_DIRECTORIES true "${_dir}/libnx-*")
		if(_found)
			set(_desnx_db "${_dir}")
		endif()
	endif()
endforeach()

set(_desnx_report "")
foreach(_entry IN LISTS DESNX_TESTED_PACKAGES)
	string(REPLACE " " ";" _entry "${_entry}")
	list(GET _entry 0 _name)
	list(GET _entry 1 _tested)
	set(_have "")
	if(_desnx_db)
		# a package's folder is <name>-<version>-<release>; the name itself may hold dashes
		file(GLOB _dirs LIST_DIRECTORIES true RELATIVE "${_desnx_db}" "${_desnx_db}/${_name}-*")
		foreach(_d IN LISTS _dirs)
			if(_d MATCHES "^${_name}-(r?[0-9][0-9.]*)-[0-9]+$")
				set(_have "${CMAKE_MATCH_1}")
			endif()
		endforeach()
	endif()
	if(NOT _have)
		list(APPEND _desnx_report "${_name} ?")
		continue()
	endif()
	list(APPEND _desnx_report "${_name} ${_have}")
	string(REGEX REPLACE "^r" "" _have_n "${_have}")
	string(REGEX REPLACE "^r" "" _tested_n "${_tested}")
	if(_have_n VERSION_LESS _tested_n)
		message(WARNING "${_name} ${_have} is older than ${_tested}, which the port was tested with: update devkitPro's packages")
	elseif(_have_n VERSION_GREATER _tested_n)
		message(STATUS "${_name} ${_have} is newer than ${_tested}, which the port was tested with")
	endif()
endforeach()
list(JOIN _desnx_report ", " _desnx_report)
if(_desnx_db)
	message(STATUS "devkitPro packages: ${_desnx_report}")
else()
	message(STATUS "devkitPro packages: no package database found, versions not checked")
endif()
