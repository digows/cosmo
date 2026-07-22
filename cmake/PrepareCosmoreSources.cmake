# PrepareCosmoreSources.cmake
#
# Generates the buildable copy of the Cosmore sources into ${COSMORE_GEN_DIR}.
#
# The submodule under vendor/cosmore is never modified. Exactly two
# transformations are applied here, and nothing else, so the diff between the
# 1992 reconstruction and what we compile stays small and auditable:
#
#   1. Drop the #include lines for Borland headers with no modern equivalent.
#   2. Comment out the 16-bit inline assembly, which the platform layer
#      reimplements.
#
# This runs in CMake rather than a shell script so it behaves identically on
# macOS, Linux and Windows without requiring sed, awk or a Python interpreter.

set(COSMORE_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/cosmore/src")
set(COSMORE_GEN_DIR "${CMAKE_BINARY_DIR}/gen")

if(NOT EXISTS "${COSMORE_SRC_DIR}")
    message(FATAL_ERROR
        "vendor/cosmore is empty. Run: git submodule update --init --recursive")
endif()

file(MAKE_DIRECTORY "${COSMORE_GEN_DIR}")

function(_cosmore_transform in_file out_file)
    file(READ "${in_file}" content)

    # 1. Borland-only headers.
    string(REGEX REPLACE
        "[ \t]*#[ \t]*include[ \t]*<(alloc|conio|dos|io|mem)\\.h>[^\n]*\n"
        "" content "${content}")

    # 2. Inline assembly. A line comment is used because several of these lines
    #    already end in a /* ... */ comment, which would nest badly.
    string(REGEX REPLACE "\n([ \t]*)asm[ \t]+" "\n\\1// ASM: " content "${content}")

    file(WRITE "${out_file}" "${content}")
endfunction()

file(GLOB _cosmore_inputs "${COSMORE_SRC_DIR}/*.c" "${COSMORE_SRC_DIR}/*.h")
foreach(_in IN LISTS _cosmore_inputs)
    get_filename_component(_name "${_in}" NAME)
    _cosmore_transform("${_in}" "${COSMORE_GEN_DIR}/${_name}")
endforeach()

# lowlevel.c is not a file in the repository: upstream publishes a pure C
# implementation of every assembly drawing routine inside C-DRAWING.md. Lifting
# it out is what frees this port from needing Turbo Assembler, which Borland
# never released for free.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/vendor/cosmore/C-DRAWING.md" _cdrawing)
string(REGEX MATCH "\n```c\n([^`]*)" _match "${_cdrawing}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "Could not extract the C drawing routines from C-DRAWING.md")
endif()
file(WRITE "${COSMORE_GEN_DIR}/lowlevel.c" "${CMAKE_MATCH_1}")

set(COSMORE_GEN_SOURCES
    "${COSMORE_GEN_DIR}/main.c"
    "${COSMORE_GEN_DIR}/game1.c"
    "${COSMORE_GEN_DIR}/game2.c"
    "${COSMORE_GEN_DIR}/lowlevel.c")

message(STATUS "Cosmore sources prepared in ${COSMORE_GEN_DIR}")
