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
option(COSMO_BUILD_GAME "Build the game itself from the Cosmore sources" ON)

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

if(COSMO_BUILD_GAME AND NOT MSVC)
    include(cmake/PrepareCosmoreSources.cmake)

    # The reconstructed 1992 sources. Warnings are silenced: this is not code
    # we get to fix, and the noise would bury real problems in our own layer.
    add_library(cosmore_game OBJECT ${COSMORE_GEN_SOURCES})
    target_include_directories(cosmore_game PRIVATE "${COSMORE_GEN_DIR}" include)
    target_compile_definitions(cosmore_game PRIVATE EPISODE=1)
    target_compile_options(cosmore_game PRIVATE
        -w -include "${CMAKE_CURRENT_SOURCE_DIR}/include/cosmo/dos_compat.h")

    # Our replacements for the parts that spoke to hardware. These see the
    # game's headers, so their prototypes match exactly, but they are ours and
    # are compiled with warnings on.
    add_library(cosmo_dos STATIC
        src/platform/lowlevel_ega.c
        src/platform/dos_compat.c
        src/platform/ports.c
        src/platform/interrupts.c
        src/platform/platform_main.c)
    target_include_directories(cosmo_dos PRIVATE "${COSMORE_GEN_DIR}")
    target_compile_definitions(cosmo_dos PRIVATE EPISODE=1)
    target_compile_options(cosmo_dos PRIVATE
        -include "${CMAKE_CURRENT_SOURCE_DIR}/include/cosmo/dos_compat.h")
    target_link_libraries(cosmo_dos PUBLIC cosmo_video PRIVATE cosmo_warnings)
    add_dependencies(cosmo_dos cosmore_game)

    add_executable(cosmo $<TARGET_OBJECTS:cosmore_game>)
    target_link_libraries(cosmo PRIVATE cosmo_dos)
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
