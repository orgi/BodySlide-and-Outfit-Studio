# Builds CBPC for the leveled list previewer from the source archive of CBPC's
# Nexus page, unpacked into lib/CBPC. CBPC's license doesn't allow uploading
# that source, so lib/CBPC is not part of the repository; without it the
# previewer is built without CBPC. See src/components/agents.md.
#
# Include after fsmp.cmake (CBPC uses its PPL stand-ins).

set(CBPC_DIR ${CMAKE_CURRENT_LIST_DIR}/../../lib/CBPC)
set(CBPC_STANDALONE_DIR ${CMAKE_CURRENT_LIST_DIR})
set(CBPC_BUILD_DIR ${CMAKE_BINARY_DIR}/cbpc)

# The archive unpacked as it is (one folder holding skse64/CBPC) or its content.
file(GLOB _cbpc_found "${CBPC_DIR}/skse64/CBPC/Thing.cpp" "${CBPC_DIR}/*/skse64/CBPC/Thing.cpp")
list(LENGTH _cbpc_found _cbpc_count)
if(_cbpc_count GREATER 1)
	message(FATAL_ERROR "CBPC: lib/CBPC holds more than one CBPC source: ${_cbpc_found}")
endif()
if(_cbpc_count EQUAL 0)
	message(STATUS "CBPC: no CBPC source in lib/CBPC; the previewer is built without CBPC")
	add_library(cbpc STATIC ${CBPC_STANDALONE_DIR}/CbpcUnavailable.cpp)
	target_include_directories(cbpc PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../lib/nifly/include)
	target_include_directories(cbpc INTERFACE ${CBPC_STANDALONE_DIR})
	return()
endif()
get_filename_component(CBPC_PLUGIN_DIR ${_cbpc_found} DIRECTORY)
get_filename_component(CBPC_SKSE_DIR ${CBPC_PLUGIN_DIR} DIRECTORY)

# ---------------------------------------------------------------------------
# CBPC's files, copied with their Visual C++ syntax made standard C++
# ---------------------------------------------------------------------------

# Writes `text` to `path` if it differs from what is there.
function(cbpc_write path text)
	if(EXISTS ${path})
		file(READ ${path} _old)
		if(_old STREQUAL text)
			return()
		endif()
	endif()
	file(WRITE ${path} "${text}")
endfunction()

# The rewrites leave everything else as it is:
#  - backslashes in #include paths become slashes;
#  - `for each (x in list)` becomes `for (x : list)`;
#  - `T::T(...)` calls, a constructor named like a function, become `T(...)`;
#  - std::ifstream becomes cbpc::GameFileStream (CbpcGame.h), which finds the
#    game paths CBPC builds the way Windows does.
function(cbpc_rewrite source destination includes_var)
	file(READ ${source} _text)

	string(REGEX MATCHALL "#[ \t]*include[ \t]*[<\"][^>\"\r\n]*[>\"]" _includes "${_text}")
	set(_found)
	foreach(_include IN LISTS _includes)
		string(REPLACE "\\" "/" _fixed "${_include}")
		if(NOT _fixed STREQUAL _include)
			string(REPLACE "${_include}" "${_fixed}" _text "${_text}")
		endif()
		string(REGEX REPLACE "^#[ \t]*include[ \t]*[<\"]([^>\"]*)[>\"]$" "\\1" _name "${_fixed}")
		list(APPEND _found ${_name})
	endforeach()

	string(REGEX REPLACE "for[ \t]+each[ \t]*\\(([^)\r\n]*)[ \t]in[ \t]([^)\r\n]*)\\)" "for (\\1 : \\2)" _text "${_text}")

	string(REGEX MATCHALL "[A-Za-z_][A-Za-z0-9_]*::[A-Za-z_][A-Za-z0-9_]*[ \t]*\\(" _calls "${_text}")
	list(REMOVE_DUPLICATES _calls)
	foreach(_call IN LISTS _calls)
		string(REGEX REPLACE "^([A-Za-z0-9_]*)::([A-Za-z0-9_]*).*$" "\\1;\\2" _parts "${_call}")
		list(GET _parts 0 _class)
		list(GET _parts 1 _member)
		if(_class STREQUAL _member)
			# Definitions (at the start of a line) stay; calls follow an operator or bracket.
			string(REGEX REPLACE "([=(,][ \t]*)${_class}::${_class}([ \t]*\\()" "\\1${_class}\\2" _text "${_text}")
		endif()
	endforeach()

	string(REPLACE "std::ifstream" "cbpc::GameFileStream" _text "${_text}")

	cbpc_write(${destination} "${_text}")
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${source})
	set(${includes_var} ${_found} PARENT_SCOPE)
endfunction()

# The game integration (the plugin's load, its hooks) is emulated by CbpcScene.cpp.
set(CBPC_GAME_INTEGRATION main.cpp hook.cpp hookD3D.cpp)
# SKSE's Win32 implementation of dirent.h; include/dirent.h stands in for it.
set(CBPC_REPLACED_HEADERS dirent.h)

file(GLOB _cbpc_plugin_files RELATIVE ${CBPC_PLUGIN_DIR} ${CBPC_PLUGIN_DIR}/*.cpp ${CBPC_PLUGIN_DIR}/*.h ${CBPC_PLUGIN_DIR}/*.hpp)
set(CBPC_SOURCES)
set(_cbpc_includes)
foreach(_file IN LISTS _cbpc_plugin_files)
	if(_file IN_LIST CBPC_GAME_INTEGRATION OR _file IN_LIST CBPC_REPLACED_HEADERS)
		continue()
	endif()
	cbpc_rewrite(${CBPC_PLUGIN_DIR}/${_file} ${CBPC_BUILD_DIR}/src/${_file} _found)
	list(APPEND _cbpc_includes ${_found})
	if(_file MATCHES "\\.cpp$")
		list(APPEND CBPC_SOURCES ${CBPC_BUILD_DIR}/src/${_file})
	endif()
endforeach()

# SKSE's files that don't depend on the game: its math types and the version
# numbers that choose CBPC's code paths.
set(CBPC_SKSE_FILES skse64/NiTypes.h skse64/NiTypes.cpp skse64_common/skse_version.h)
foreach(_file IN LISTS CBPC_SKSE_FILES)
	if(_file MATCHES "\\.cpp$")
		cbpc_rewrite(${CBPC_SKSE_DIR}/${_file} ${CBPC_BUILD_DIR}/src/${_file} _found)
		list(APPEND CBPC_SOURCES ${CBPC_BUILD_DIR}/src/${_file})
	else()
		cbpc_rewrite(${CBPC_SKSE_DIR}/${_file} ${CBPC_BUILD_DIR}/include/${_file} _found)
	endif()
	list(APPEND _cbpc_includes ${_found})
endforeach()

# Every other SKSE header CBPC includes is include/CbpcGame.h.
list(REMOVE_DUPLICATES _cbpc_includes)
foreach(_include IN LISTS _cbpc_includes)
	if(NOT _include MATCHES "^(skse64|skse64_common|common)/")
		continue()
	endif()
	if(EXISTS ${CBPC_STANDALONE_DIR}/include/${_include} OR _include IN_LIST CBPC_SKSE_FILES)
		continue()
	endif()
	cbpc_write(${CBPC_BUILD_DIR}/include/${_include} "// Generated by cbpc.cmake: SKSE's ${_include} for CBPC.\n#pragma once\n#include \"CbpcGame.h\"\n")
endforeach()

# CBPC's version and the game version its project is built for.
file(READ ${CBPC_PLUGIN_DIR}/config.cpp _cbpc_config)
string(REGEX MATCH "versionStr = \"([^\"]*)\"" _cbpc_version "${_cbpc_config}")
set(CBPC_VERSION ${CMAKE_MATCH_1})
file(READ ${CBPC_SKSE_DIR}/sheets/Runtime.props _cbpc_runtime_props)
string(REGEX MATCH "RUNTIME_VERSION=(0x[0-9A-Fa-f]+)" _cbpc_runtime "${_cbpc_runtime_props}")
if(NOT _cbpc_runtime)
	message(FATAL_ERROR "CBPC: RUNTIME_VERSION not found in ${CBPC_SKSE_DIR}/sheets/Runtime.props")
endif()
set(CBPC_RUNTIME_VERSION ${CMAKE_MATCH_1})
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CBPC_SKSE_DIR}/sheets/Runtime.props)
message(STATUS "CBPC: version ${CBPC_VERSION} from lib/CBPC")

# ---------------------------------------------------------------------------
# CBPC
# ---------------------------------------------------------------------------

set(CBPC_STANDALONE_SOURCES
	${CBPC_STANDALONE_DIR}/CbpcScene.cpp
	${CBPC_STANDALONE_DIR}/StandIns.cpp
	)

add_library(cbpc STATIC ${CBPC_SOURCES} ${CBPC_STANDALONE_SOURCES})
target_compile_features(cbpc PRIVATE cxx_std_17)
target_compile_definitions(cbpc PRIVATE RUNTIME RUNTIME_VERSION=${CBPC_RUNTIME_VERSION})
# CbpcScene.cpp includes CBPC's headers; they are third-party code like CBPC's files.
target_include_directories(cbpc SYSTEM PRIVATE ${CBPC_BUILD_DIR}/src)
target_include_directories(cbpc PRIVATE
	${CBPC_STANDALONE_DIR}/include
	${CBPC_BUILD_DIR}/include
	${CMAKE_CURRENT_LIST_DIR}/../../lib/nifly/include
	)
if(NOT MSVC)
	target_include_directories(cbpc PRIVATE ${FSMP_STANDALONE_DIR}/include-noppl)
endif()
if(NOT WIN32)
	target_include_directories(cbpc PRIVATE ${CBPC_STANDALONE_DIR}/include-nowin)
endif()
target_include_directories(cbpc INTERFACE ${CBPC_STANDALONE_DIR})

# Visual C++ gives each global variable a symbol that includes its type; two of
# CBPC's files define these names with different types, each file using its
# own. Other compilers get the ones of the first file renamed (file:name).
set(CBPC_SAME_NAMED_GLOBALS SimObj.cpp:pelvis SimObj.cpp:anal)
if(NOT MSVC)
	foreach(_global IN LISTS CBPC_SAME_NAMED_GLOBALS)
		string(REPLACE ":" ";" _parts ${_global})
		list(GET _parts 0 _file)
		list(GET _parts 1 _name)
		get_filename_component(_stem ${_file} NAME_WE)
		set_property(SOURCE ${CBPC_BUILD_DIR}/src/${_file} APPEND PROPERTY COMPILE_DEFINITIONS "${_name}=${_stem}_${_name}")
	endforeach()
endif()

# CBPC's project includes SKSE's prefix header first in every file. CBPC's
# files are compiled as they are, without their warnings.
if(MSVC)
	set_source_files_properties(${CBPC_SOURCES} PROPERTIES COMPILE_OPTIONS "/FI${CBPC_STANDALONE_DIR}/include/common/IPrefix.h;/W0")
else()
	set_source_files_properties(${CBPC_SOURCES} PROPERTIES COMPILE_OPTIONS "-include;${CBPC_STANDALONE_DIR}/include/common/IPrefix.h;-w")
endif()
