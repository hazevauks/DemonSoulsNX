# uam, devkitPro's deko3D shader compiler (GLSL to DKSH), built for the Switch port (DemonSoulsNX,
# stage 12c: the game's shaders are compiled on the console, src/gpu/gpu_dk_sc.cpp).
#
# The sources are uam's meson.build's: every .c and .cpp of mesa-imported, and source/ but its
# main.cpp. Its parsers (bison, flex) and the three ir_expression_operation headers (Mesa's
# Python generator) were made once and are kept beside their inputs (third_party/README.md), so
# the build runs none of those tools. Not part of meson.build: DESNX_UAM_LIBRARY, which keeps the
# GLSL front end's context, types and built-ins from one compile to the next.
#
# The game links switch-mesa's EGL too (SDL2 needs it), which holds a Mesa GLSL compiler of its
# own under the same names. So uam is compiled with its symbols hidden but its DekoCompiler class,
# put together into one relocatable object (ld -r), every symbol it defines but that class's
# renamed uam_<name> (uam_rename.cmake: C++'s COMDAT groups are kept one per name, so a local
# copy alone would take EGL's place) and the hidden ones made local: UAM_OBJECT, for the
# program's sources, with uam_lib for its headers.

file(GLOB_RECURSE UAM_MESA_SOURCES CONFIGURE_DEPENDS
	${CMAKE_CURRENT_SOURCE_DIR}/mesa-imported/*.c ${CMAKE_CURRENT_SOURCE_DIR}/mesa-imported/*.cpp)
add_library(uam_obj OBJECT ${UAM_MESA_SOURCES}
	${CMAKE_CURRENT_SOURCE_DIR}/source/compiler_iface.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/source/glsl_frontend.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/source/mini-os.c
	${CMAKE_CURRENT_SOURCE_DIR}/source/tgsi_support.cpp)
set_target_properties(uam_obj PROPERTIES C_STANDARD 99 C_EXTENSIONS ON CXX_STANDARD 11 CXX_EXTENSIONS ON)
target_include_directories(uam_obj PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/mesa-imported)
target_compile_definitions(uam_obj PRIVATE
	DESKTOP _USE_MATH_DEFINES "PACKAGE_STRING=\"uam 1.1.0\"" _GNU_SOURCE DESNX_UAM_LIBRARY)
# third-party code, as its own build has it: no warnings of ours
target_compile_options(uam_obj PRIVATE -w -fno-strict-aliasing -fvisibility=hidden -fvisibility-inlines-hidden)

# (the objects as arguments, not in a response file: the devkitPro shell turns its own paths into
# Windows' on a command line, and inside a file nothing does)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/uam.o
	COMMAND ${CMAKE_LINKER} -r -o uam_all.o $<TARGET_OBJECTS:uam_obj>
	COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM} -DIN=uam_all.o -DOUT=uam_rename.txt
		-P ${CMAKE_CURRENT_SOURCE_DIR}/uam_rename.cmake
	COMMAND ${CMAKE_OBJCOPY} --redefine-syms=uam_rename.txt --localize-hidden uam_all.o uam.o
	DEPENDS uam_obj $<TARGET_OBJECTS:uam_obj> ${CMAKE_CURRENT_SOURCE_DIR}/uam_rename.cmake
	WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
	COMMENT "Putting uam into one object, its Mesa copy local to it"
	COMMAND_EXPAND_LISTS VERBATIM)
add_custom_target(uam_link DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/uam.o)
set(UAM_OBJECT ${CMAKE_CURRENT_BINARY_DIR}/uam.o PARENT_SCOPE)

add_library(uam_lib INTERFACE)
target_include_directories(uam_lib INTERFACE
	${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_CURRENT_SOURCE_DIR}/mesa-imported)
target_compile_definitions(uam_lib INTERFACE DESKTOP _USE_MATH_DEFINES)
add_dependencies(uam_lib uam_link)
