cmake_minimum_required(VERSION 3.21)

project(cosmo
    VERSION 0.1.0
    DESCRIPTION "Native port of Cosmo's Cosmic Adventure (Apogee, 1992)"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(COSMO_WERROR "Treat warnings as errors" OFF)
option(COSMO_FETCH_SDL "Always build SDL3 from source instead of using a system copy" OFF)
option(COSMO_BUILD_TESTS "Build the test suite" ON)
option(COSMO_BUILD_GAME_OBJECTS
    "Compile the Cosmore game sources (not linkable yet)" ON)

# --------------------------------------------------------------------------
# Warnings
# --------------------------------------------------------------------------

add_library(cosmo_warnings INTERFACE)
if(MSVC)
    target_compile_options(cosmo_warnings INTERFACE /W4)
    if(COSMO_WERROR)
        target_compile_options(cosmo_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(cosmo_warnings INTERFACE -Wall -Wextra)
    if(COSMO_WERROR)
        target_compile_options(cosmo_warnings INTERFACE -Werror)
    endif()
endif()

# --------------------------------------------------------------------------
# SDL3
# --------------------------------------------------------------------------

# A system SDL3 is preferred, but it is usually single-architecture. Multi-arch
# builds (macOS universal, cross-compiles) must build SDL from source instead.
if(NOT COSMO_FETCH_SDL)
    list(LENGTH CMAKE_OSX_ARCHITECTURES _osx_arch_count)
    if(_osx_arch_count GREATER 1)
        message(STATUS "Multiple target architectures requested; "
                       "building SDL3 from source")
        set(COSMO_FETCH_SDL ON)
    else()
        find_package(SDL3 CONFIG QUIET)
    endif()
endif()

if(NOT SDL3_FOUND)
    message(STATUS "Building SDL3 from source")
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.12
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(SDL3)
else()
    message(STATUS "Using system SDL3 ${SDL3_VERSION}")
endif()

# --------------------------------------------------------------------------
# Libraries
# --------------------------------------------------------------------------

# Core has no windowing dependency, so the tests can link it on a headless
# machine without pulling SDL in.
add_library(cosmo_core STATIC
    src/platform/ega.c
    src/platform/png.c)
target_include_directories(cosmo_core PUBLIC include)
target_link_libraries(cosmo_core PRIVATE cosmo_warnings)

add_library(cosmo_video STATIC
    src/platform/video.c)
target_link_libraries(cosmo_video
    PUBLIC cosmo_core SDL3::SDL3
    PRIVATE cosmo_warnings)

# --------------------------------------------------------------------------
# Cosmore game sources
# --------------------------------------------------------------------------
#
# These compile but do not link yet: the platform layer still owes them the
# interrupt, timing, keyboard and audio services. Building them anyway keeps
# CI honest about whether the port still compiles on every platform.

if(COSMO_BUILD_GAME_OBJECTS AND NOT MSVC)
    include(cmake/PrepareCosmoreSources.cmake)

    add_library(cosmore_objects OBJECT ${COSMORE_GEN_SOURCES})
    target_include_directories(cosmore_objects PRIVATE
        "${COSMORE_GEN_DIR}"
        include
        include/cosmo)
    target_compile_definitions(cosmore_objects PRIVATE EPISODE=1)
    target_compile_options(cosmore_objects PRIVATE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/include/cosmo/dos_compat.h")
    # This is 1992 code reconstructed verbatim; its warnings are not ours to
    # fix, and silencing them keeps real problems visible elsewhere.
    target_compile_options(cosmore_objects PRIVATE -w)
endif()

# --------------------------------------------------------------------------
# Tools
# --------------------------------------------------------------------------

add_executable(imgview tools/imgview.c)
target_link_libraries(imgview PRIVATE cosmo_video cosmo_warnings)

# --------------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------------

if(COSMO_BUILD_TESTS)
    enable_testing()
    add_executable(test_ega tests/test_ega.c)
    target_link_libraries(test_ega PRIVATE cosmo_core cosmo_warnings)
    add_test(NAME ega COMMAND test_ega)
endif()
