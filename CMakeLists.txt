cmake_minimum_required(VERSION 3.21)

project(cosmo
    VERSION 0.1.0
    DESCRIPTION "Native port of Cosmo's Cosmic Adventure (Apogee, 1992)"
    LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# C++ is here for exactly one reason: ymfm, the YM3812 synthesiser.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
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

    # Spellings for the two flags the vendored code needs. MSVC writes both
    # differently, and hard-coding the GCC forms is what kept the game from
    # being built on Windows at all.
    set(COSMO_NO_WARNINGS /w)
    set(COSMO_FORCE_INCLUDE /FI)
else()
    target_compile_options(cosmo_warnings INTERFACE -Wall -Wextra)
    if(COSMO_WERROR)
        target_compile_options(cosmo_warnings INTERFACE -Werror)
    endif()

    set(COSMO_NO_WARNINGS -w)
    set(COSMO_FORCE_INCLUDE -include)
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
# ymfm -- the AdLib's YM3812
# --------------------------------------------------------------------------

# Only the OPL core is built. ymfm covers Yamaha's whole OPx family, and the
# rest of it has no bearing on a 1992 AdLib. Its warnings are not ours to fix.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/ymfm/src/ymfm_opl.cpp")
    message(FATAL_ERROR
        "vendor/ymfm is empty. Run: git submodule update --init --recursive")
endif()

add_library(ymfm STATIC
    vendor/ymfm/src/ymfm_opl.cpp
    vendor/ymfm/src/ymfm_adpcm.cpp
    vendor/ymfm/src/ymfm_pcm.cpp)
# SYSTEM so that warnings from ymfm's headers do not reach our -Werror build.
target_include_directories(ymfm SYSTEM PUBLIC vendor/ymfm/src)
target_compile_options(ymfm PRIVATE ${COSMO_NO_WARNINGS})

# --------------------------------------------------------------------------
# Libraries
# --------------------------------------------------------------------------

# Core has no windowing dependency, so the tests can link it on a headless
# machine without pulling SDL in.
add_library(cosmo_core STATIC
    src/platform/ega.c
    src/platform/png.c
    src/platform/group.c
    src/platform/font.c)
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

if(COSMO_BUILD_GAME)
    include(cmake/PrepareCosmoreSources.cmake)

    # Everything that does not depend on which episode is being built. The
    # platform layer reaches the game through a handful of externs rather than
    # its headers, so it compiles once and links into all three.
    add_library(cosmo_platform STATIC
        src/platform/dos_compat.c
        src/platform/ports.c
        src/platform/interrupts.c
        src/platform/audio.c
        src/platform/opl.cpp
        src/platform/input.c
        src/platform/platform_main.c)
    target_link_libraries(cosmo_platform
        PUBLIC cosmo_video
        PRIVATE ymfm cosmo_warnings)

    #
    # One executable per episode, which is not a packaging choice: the episodes
    # differ by preprocessor conditionals that include or exclude whole actor
    # implementations -- HAS_ACT_BOSS, HAS_ACT_RED_JUMPER, EXPLOSION_PALETTE and
    # others -- so they genuinely compile to different programs. Apogee shipped
    # three executables for the same reason. The launcher below puts a single
    # front door on them.
    #
    function(cosmo_add_episode episode)
        add_library(cosmore_game${episode} OBJECT ${COSMORE_GEN_SOURCES})
        target_include_directories(cosmore_game${episode}
            PRIVATE "${COSMORE_GEN_DIR}" include)
        target_compile_definitions(cosmore_game${episode} PRIVATE EPISODE=${episode})
        # Reconstructed 1992 code; its warnings are not ours to fix, and the
        # noise would bury real problems in our own layer.
        target_compile_options(cosmore_game${episode} PRIVATE
            ${COSMO_NO_WARNINGS}
            ${COSMO_FORCE_INCLUDE} "${CMAKE_CURRENT_SOURCE_DIR}/include/cosmo/dos_compat.h")

        # The drawing routines see the game's headers, so they are episode
        # specific too, but they are ours and are built with warnings on.
        add_library(cosmo_lowlevel${episode} STATIC src/platform/lowlevel_ega.c)
        target_include_directories(cosmo_lowlevel${episode}
            PRIVATE "${COSMORE_GEN_DIR}")
        target_compile_definitions(cosmo_lowlevel${episode} PRIVATE EPISODE=${episode})
        target_compile_options(cosmo_lowlevel${episode} PRIVATE
            ${COSMO_FORCE_INCLUDE} "${CMAKE_CURRENT_SOURCE_DIR}/include/cosmo/dos_compat.h")
        target_link_libraries(cosmo_lowlevel${episode}
            PUBLIC cosmo_core PRIVATE cosmo_warnings)

        add_executable(cosmo${episode} $<TARGET_OBJECTS:cosmore_game${episode}>)
        target_link_libraries(cosmo${episode}
            PRIVATE cosmo_lowlevel${episode} cosmo_platform)
        add_dependencies(cosmo${episode} cosmore_game${episode})
    endfunction()

    cosmo_add_episode(1)
    cosmo_add_episode(2)
    cosmo_add_episode(3)

    # The front door: finds which episodes have data present and runs one.
    add_executable(cosmo src/launcher.c)
    target_link_libraries(cosmo PRIVATE cosmo_video cosmo_warnings)
    add_dependencies(cosmo cosmo1 cosmo2 cosmo3)
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
