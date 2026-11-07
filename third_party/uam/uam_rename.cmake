# uam_rename.cmake (DemonSoulsNX) -- run as cmake -DNM=<nm> -DIN=<uam_all.o> -DOUT=<list> -P:
# writes objcopy's --redefine-syms list for uam's one object, every symbol it defines but its
# DekoCompiler class's renamed uam_<name>. So its Mesa copy shares no name with switch-mesa's,
# not even the COMDAT groups C++ makes for inline definitions, which the linker keeps one of by
# name (third_party/uam/CMakeLists.txt).
execute_process(COMMAND ${NM} --defined-only -g ${IN} OUTPUT_VARIABLE syms RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
	message(FATAL_ERROR "nm failed on ${IN}")
endif()
string(REPLACE "\n" ";" lines "${syms}")
set(out "")
foreach(line IN LISTS lines)
	if(line MATCHES "^[0-9a-fA-F]* *[A-Za-z] (.+)$")
		set(name "${CMAKE_MATCH_1}")
		if(NOT name MATCHES "DekoCompiler")
			string(APPEND out "${name} uam_${name}\n")
		endif()
	endif()
endforeach()
file(WRITE ${OUT} "${out}")
