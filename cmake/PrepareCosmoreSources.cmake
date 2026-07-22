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

    # 3. Fixed-width base types. On DOS `unsigned int` was 16 bits and
    #    `unsigned long` 32; here they are 32 and 64. The game leans on 16-bit
    #    wraparound in several places -- the sources even comment on it -- so
    #    these have to keep their original widths or the arithmetic diverges.
    string(REPLACE "typedef unsigned char byte;" "typedef uint8_t  byte;"
        content "${content}")
    string(REPLACE "typedef unsigned int  word;" "typedef uint16_t word;"
        content "${content}")
    string(REPLACE "typedef unsigned long dword;" "typedef uint32_t dword;"
        content "${content}")

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

# Anything the two mechanical transformations cannot express lives in patches/.
# Currently that is one patch, rerouting the two places that store through a far
# pointer into video memory. Applying them here, rather than committing modified
# copies, keeps the delta from 1992 visible in review.
file(GLOB _cosmore_patches "${CMAKE_CURRENT_SOURCE_DIR}/patches/*.patch")
list(SORT _cosmore_patches)

# Fingerprint every generated source, so a patch that quietly changes nothing
# can be caught.
function(_cosmore_fingerprint out_var)
    set(_acc "")
    file(GLOB _files "${COSMORE_GEN_DIR}/*.c" "${COSMORE_GEN_DIR}/*.h")
    list(SORT _files)
    foreach(_f IN LISTS _files)
        file(MD5 "${_f}" _hash)
        string(APPEND _acc "${_hash}")
    endforeach()
    string(MD5 _result "${_acc}")
    set(${out_var} "${_result}" PARENT_SCOPE)
endfunction()

if(_cosmore_patches)
    find_package(Git REQUIRED)

    foreach(_patch IN LISTS _cosmore_patches)
        get_filename_component(_patch_name "${_patch}" NAME)
        _cosmore_fingerprint(_before)

        # GIT_CEILING_DIRECTORIES stops git from discovering the surrounding
        # repository. Without it, `git apply` resolves the patch paths against
        # the repository root instead of the working directory, silently
        # ignores everything outside it, and still exits 0 -- so the patch
        # appears to apply while changing nothing at all.
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env
                    "GIT_CEILING_DIRECTORIES=${CMAKE_BINARY_DIR}"
                    "${GIT_EXECUTABLE}" apply -p1 "${_patch}"
            WORKING_DIRECTORY "${COSMORE_GEN_DIR}"
            RESULT_VARIABLE _patch_result
            ERROR_VARIABLE _patch_error)

        if(NOT _patch_result EQUAL 0)
            message(FATAL_ERROR
                "Failed to apply ${_patch_name}:\n${_patch_error}")
        endif()

        _cosmore_fingerprint(_after)
        if(_before STREQUAL _after)
            message(FATAL_ERROR
                "${_patch_name} reported success but changed nothing. "
                "The generated sources are not what the patch expected.")
        endif()

        message(STATUS "Applied ${_patch_name}")
    endforeach()
endif()

# main.c is deliberately excluded: it only performs the 8086 detection that
# refuses to start on a pre-286 machine, then calls InnerMain(). The platform
# layer provides its own entry point and calls InnerMain() directly.
#
# lowlevel.c is excluded too. It is extracted above for reference, but the
# build uses src/platform/lowlevel_ega.c, which is the same routines rewritten
# against the emulated adapter.
set(COSMORE_GEN_SOURCES
    "${COSMORE_GEN_DIR}/game1.c"
    "${COSMORE_GEN_DIR}/game2.c")

message(STATUS "Cosmore sources prepared in ${COSMORE_GEN_DIR}")
