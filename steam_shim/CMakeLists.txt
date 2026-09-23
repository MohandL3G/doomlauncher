cmake_minimum_required(VERSION 3.15)
project(steam_api64_shim C)

# The shim is a pure MSVC build: the .def file uses MSVC linker syntax
# (export forwarding) and the code is plain Win32.
if(NOT MSVC)
    message(FATAL_ERROR "steam_api64 shim requires MSVC (def-file export forwarding)")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "steam_api64 shim must be built for x64")
endif()

add_library(steam_api64 SHARED steam_api64.c steam_api64.def)

target_compile_definitions(steam_api64 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_link_libraries(steam_api64 PRIVATE kernel32)

# Small, self-contained, release-grade: favor size and startup cost.
target_compile_options(steam_api64 PRIVATE
    $<$<CONFIG:Release>:/O1 /Os /Gy /GF /Zc:wchar_t>
)
target_link_options(steam_api64 PRIVATE
    $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>
    /SUBSYSTEM:WINDOWS
)

set_target_properties(steam_api64 PROPERTIES
    OUTPUT_NAME "steam_api64"
    PREFIX ""
)
