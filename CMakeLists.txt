cmake_minimum_required(VERSION 3.21)

project(cosmo
    VERSION 0.3.0
    DESCRIPTION "Native port of Cosmo's Cosmic Adventure (Apogee, 1992)"
    LANGUAGES C CXX)

if(IOS)
    enable_language(OBJCXX)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# C++ is here for exactly one reason: ymfm, the YM3812 synthesiser.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The Android debug APK is the build used for on-device play testing. Keep its
# native code optimized so the game loop and MediaCodec PCM conversion do not
# compete with the audio callback on mobile CPUs.
if(ANDROID)
    add_compile_options($<$<CONFIG:Debug>:-O2>)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(COSMO_WERROR "Treat warnings as errors" OFF)
option(COSMO_FETCH_SDL "Always build SDL3 from source instead of using a system copy" OFF)
option(COSMO_BUILD_TESTS "Build the test suite" ON)
option(COSMO_BUILD_GAME "Build the game itself from the Cosmore sources" ON)
option(COSMO_BUNDLE "Build as an application rather than a terminal program" OFF)
option(COSMO_WIDESCREEN "Enable the widescreen mod by default for preview builds" OFF)
option(COSMO_ANDROID_APP "Build Episode 1 as an SDL Android shared library" OFF)
option(COSMO_DEV_LEVEL_SHORTCUTS "Enable developer level shortcuts" OFF)
option(COSMO_TOUCH_PREVIEW "Enable the Android touch-control preview" OFF)
option(COSMO_IOS_APP "Build Episode 1 as an SDL iOS application bundle" OFF)
option(COSMO_VITA_APP "Build a PlayStation Vita homebrew VPK" OFF)

if(COSMO_VITA_APP)
    if(NOT VITA)
        message(FATAL_ERROR "COSMO_VITA_APP requires the VitaSDK toolchain")
    endif()

    include("$ENV{VITASDK}/share/vita.cmake")
    set(VITA_APP_NAME "Cosmo Enhanced")
    set(VITA_TITLEID "CSMO00001")
    set(VITA_VERSION "01.00")
endif()

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
    if(ANDROID)
        # SDLActivity loads SDL3 and main as Android shared libraries.
        set(SDL_SHARED ON CACHE BOOL "" FORCE)
        set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    else()
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
    endif()
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
# Application packaging
# --------------------------------------------------------------------------
#
# COSMO_BUNDLE turns the game from something started in a terminal into
# something double-clicked: a bundle with an icon on macOS, an icon and no
# console window on Windows. It is off by default, because a plain binary is
# easier to work on, and on in CI so that what is tested is what ships.

set(COSMO_ICON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")

if(COSMO_BUNDLE AND WIN32)
    # A resource script is how an icon gets into a Windows executable. It is
    # generated rather than committed so the path stays right.
    set(COSMO_WIN_ICON_RC "${CMAKE_BINARY_DIR}/cosmo_icon.rc")
    file(WRITE "${COSMO_WIN_ICON_RC}"
         "1 ICON \"${COSMO_ICON_DIR}/icon.ico\"\n")

    # MinGW links its own runtime dynamically by default, which means shipping
    # libgcc_s_seh-1.dll, libstdc++-6.dll and libwinpthread-1.dll beside the
    # game -- and a dialog about a missing DLL for anyone who unpacks only the
    # executable. Linking them in avoids the whole subject.
    if(MINGW)
        add_link_options(-static-libgcc -static-libstdc++ -static)
    endif()
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

# paths.c belongs here rather than in core: working out where an application's
# data lives means asking SDL, and core is kept free of it.
if(WIN32)
    # The launcher and episode modules share this one runtime so an episode
    # can take over the launcher's existing SDL window without creating a
    # second process or top-level window. EGA state lives here too, so both
    # sides draw into the same adapter.
    add_library(cosmo_video SHARED
        src/platform/ega.c
        src/platform/png.c
        src/platform/video.c
        src/platform/paths.c
        src/platform/font.c
        src/platform/group.c
        src/platform/mods.c)
    target_include_directories(cosmo_video PUBLIC include)
    set_target_properties(cosmo_video PROPERTIES
        PREFIX ""
        WINDOWS_EXPORT_ALL_SYMBOLS ON)
    target_link_libraries(cosmo_video
        PUBLIC SDL3::SDL3
        PRIVATE cosmo_warnings)
else()
    add_library(cosmo_video STATIC
        src/platform/video.c
        src/platform/paths.c
        src/platform/font.c
        src/platform/group.c
        src/platform/mods.c)
    target_link_libraries(cosmo_video
        PUBLIC cosmo_core SDL3::SDL3
        PRIVATE cosmo_warnings)
endif()
if(COSMO_WIDESCREEN)
    target_compile_definitions(cosmo_video PRIVATE COSMO_DEFAULT_WIDESCREEN)
endif()
if(COSMO_IOS_APP)
    target_compile_definitions(cosmo_video PRIVATE COSMO_IOS_APP=1)
endif()
if(COSMO_VITA_APP)
    target_compile_definitions(cosmo_video PRIVATE COSMO_VITA_APP=1)
endif()

# --------------------------------------------------------------------------
# Cosmore game sources
# --------------------------------------------------------------------------
#
# These compile but do not link yet: the platform layer still owes them the
# interrupt, timing, keyboard and audio services. Building them anyway keeps
# CI honest about whether the port still compiles on every platform.

if(COSMO_BUILD_GAME)
    include(cmake/PrepareCosmoreSources.cmake)
    # Stage all bundled mods for source-run builds, not only Widescreen.
    # Track additions and content changes so regeneration keeps the build copy
    # in sync with the source package.
    file(GLOB_RECURSE COSMO_MOD_FILES CONFIGURE_DEPENDS LIST_DIRECTORIES false
         "${CMAKE_CURRENT_SOURCE_DIR}/Mods/*")
    foreach(COSMO_MOD_FILE IN LISTS COSMO_MOD_FILES)
        file(RELATIVE_PATH COSMO_MOD_RELATIVE_PATH
             "${CMAKE_CURRENT_SOURCE_DIR}/Mods" "${COSMO_MOD_FILE}")
        get_filename_component(COSMO_MOD_DESTINATION
             "${CMAKE_BINARY_DIR}/Mods/${COSMO_MOD_RELATIVE_PATH}" DIRECTORY)
        file(MAKE_DIRECTORY "${COSMO_MOD_DESTINATION}")
        configure_file("${COSMO_MOD_FILE}"
                       "${CMAKE_BINARY_DIR}/Mods/${COSMO_MOD_RELATIVE_PATH}"
                       COPYONLY)
    endforeach()

    # Everything that does not depend on which episode is being built. The
    # platform layer reaches the game through a handful of externs rather than
    # its headers, so it compiles once and links into all three.
    add_library(cosmo_platform STATIC
        src/platform/dos_compat.c
        src/platform/ports.c
        src/platform/interrupts.c
        src/platform/audio.c
        src/platform/opl.cpp
        src/platform/input.c)
    if(COSMO_VITA_APP)
        target_compile_definitions(cosmo_platform PRIVATE COSMO_VITA_APP=1)
    endif()
    if(WIN32)
        target_sources(cosmo_platform PRIVATE src/platform/mp3_decoder.cpp)
        target_link_libraries(cosmo_platform PRIVATE mfplat mfreadwrite mfuuid ole32)
        target_compile_definitions(cosmo_platform PRIVATE COSMO_MP3_MUSIC=1)
    elseif(ANDROID)
        target_sources(cosmo_platform PRIVATE src/platform/mp3_decoder_android.cpp)
        target_link_libraries(cosmo_platform PRIVATE mediandk)
        target_compile_definitions(cosmo_platform PRIVATE COSMO_MP3_MUSIC=1)
    elseif(COSMO_VITA_APP)
        target_sources(cosmo_platform PRIVATE src/platform/mp3_decoder_vita.cpp)
        target_link_libraries(cosmo_platform PRIVATE
            SceAudiodec_stub SceSysmodule_stub)
        target_compile_definitions(cosmo_platform PRIVATE COSMO_MP3_MUSIC=1)
    elseif(IOS)
        target_sources(cosmo_platform PRIVATE src/platform/mp3_decoder_apple.mm)
        find_library(COSMO_AVFOUNDATION_FRAMEWORK AVFoundation REQUIRED)
        target_link_libraries(cosmo_platform PRIVATE ${COSMO_AVFOUNDATION_FRAMEWORK})
        target_compile_definitions(cosmo_platform PRIVATE COSMO_MP3_MUSIC=1)
        set_target_properties(cosmo_platform PROPERTIES
            XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES)
    endif()
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
        # Compile the extended renderer into every episode for the runtime mod.
        target_compile_definitions(cosmore_game${episode}
            PRIVATE EPISODE=${episode} COSMO_WIDESCREEN)
        if(COSMO_DEV_LEVEL_SHORTCUTS)
            target_compile_definitions(cosmore_game${episode} PRIVATE COSMO_DEV_LEVEL_SHORTCUTS)
        endif()
        if(UNIX OR VITA)
            # Strict C11 hides POSIX declarations such as fileno() unless a
            # POSIX feature level is selected explicitly.
            target_compile_definitions(cosmore_game${episode}
                PRIVATE _POSIX_C_SOURCE=200809L)
        endif()
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
        if(WIN32)
            target_link_libraries(cosmo_lowlevel${episode}
                PUBLIC cosmo_video PRIVATE cosmo_warnings)
        else()
            target_link_libraries(cosmo_lowlevel${episode}
                PUBLIC cosmo_core PRIVATE cosmo_warnings)
        endif()

        # Mobile app targets host the selected episode in the launcher process
        # instead of starting a sibling episode executable.
        if(COSMO_ANDROID_APP OR COSMO_IOS_APP OR COSMO_VITA_APP)
            return()
        endif()

        if(COSMO_BUNDLE AND WIN32)
            add_executable(cosmo${episode} WIN32
                $<TARGET_OBJECTS:cosmore_game${episode}>
                src/platform/platform_main.c "${COSMO_WIN_ICON_RC}")
        else()
            add_executable(cosmo${episode}
                $<TARGET_OBJECTS:cosmore_game${episode}>
                src/platform/platform_main.c)
        endif()
        target_link_libraries(cosmo${episode}
            PRIVATE cosmo_lowlevel${episode} cosmo_platform)
        add_dependencies(cosmo${episode} cosmore_game${episode})

        if(WIN32)
            add_library(cosmo${episode}_module MODULE
                $<TARGET_OBJECTS:cosmore_game${episode}>
                src/platform/platform_main.c)
            target_compile_definitions(cosmo${episode}_module
                PRIVATE COSMO_EPISODE_MODULE=1)
            target_link_libraries(cosmo${episode}_module
                PRIVATE cosmo_lowlevel${episode} cosmo_platform)
            set_target_properties(cosmo${episode}_module PROPERTIES
                PREFIX ""
                OUTPUT_NAME "cosmo${episode}"
                LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/episodes"
                RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/episodes"
                WINDOWS_EXPORT_ALL_SYMBOLS ON)
            add_dependencies(cosmo${episode}_module cosmore_game${episode})
        endif()

    endfunction()

    if(COSMO_ANDROID_APP)
        if(NOT ANDROID)
            message(FATAL_ERROR "COSMO_ANDROID_APP requires the Android NDK toolchain")
        endif()

        cosmo_add_episode(1)
        add_library(main SHARED
            $<TARGET_OBJECTS:cosmore_game1>
            src/platform/platform_main.c
            src/launcher.c)
        target_compile_definitions(main PRIVATE COSMO_EPISODE_MODULE)
        if(COSMO_TOUCH_PREVIEW)
            target_compile_definitions(main PRIVATE COSMO_TOUCH_PREVIEW)
            target_compile_definitions(cosmore_game1 PRIVATE COSMO_TOUCH_PREVIEW)
        endif()
        target_link_libraries(main PRIVATE cosmo_lowlevel1 cosmo_platform cosmo_video)
        add_dependencies(main cosmore_game1)
        foreach(episode RANGE 2 3)
            cosmo_add_episode(${episode})
            add_library(cosmo${episode} SHARED
                $<TARGET_OBJECTS:cosmore_game${episode}>
                src/platform/platform_main.c)
            target_compile_definitions(cosmo${episode} PRIVATE COSMO_EPISODE_MODULE)
            if(COSMO_TOUCH_PREVIEW)
                target_compile_definitions(cosmo${episode} PRIVATE COSMO_TOUCH_PREVIEW)
                target_compile_definitions(cosmore_game${episode} PRIVATE COSMO_TOUCH_PREVIEW)
            endif()
            # Each episode has its own game and platform state.
            target_link_options(cosmo${episode} PRIVATE -Wl,-Bsymbolic)
            target_link_libraries(cosmo${episode}
                PRIVATE cosmo_lowlevel${episode} cosmo_platform cosmo_video)
        endforeach()
        return()
    endif()

    if(COSMO_IOS_APP)
        if(NOT IOS)
            message(FATAL_ERROR "COSMO_IOS_APP requires the iOS SDK toolchain")
        endif()

        set(COSMO_IOS_EXECUTABLE_NAME "CosmoEnhanced")
        set(COSMO_IOS_BUNDLE_NAME "Cosmo Enhanced")
        set(COSMO_IOS_BUNDLE_IDENTIFIER "com.digows.cosmoenhanced")
        set(COSMO_IOS_BUNDLE_VERSION "1")
        set(COSMO_IOS_SHORT_VERSION "0.4.0")
        configure_file(ios-build/Info.plist.in
            "${CMAKE_CURRENT_BINARY_DIR}/CosmoEnhanced-Info.plist" @ONLY)

        # iOS, like Android, runs one episode inside the launcher process. SDL3
        # supplies the UIKit application entry point through SDL_main.h.
        cosmo_add_episode(1)
        add_executable(cosmo_ios MACOSX_BUNDLE
            $<TARGET_OBJECTS:cosmore_game1>
            src/platform/platform_main.c
            src/launcher.c)
        target_compile_definitions(cosmo_ios PRIVATE
            COSMO_EPISODE_MODULE=1 COSMO_IOS_APP=1)
        target_link_libraries(cosmo_ios PRIVATE
            cosmo_lowlevel1 cosmo_platform cosmo_video cosmo_warnings)
        add_dependencies(cosmo_ios cosmore_game1)
        set_target_properties(cosmo_ios PROPERTIES
            OUTPUT_NAME "${COSMO_IOS_EXECUTABLE_NAME}"
            MACOSX_BUNDLE_BUNDLE_NAME "${COSMO_IOS_BUNDLE_NAME}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${COSMO_IOS_BUNDLE_IDENTIFIER}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${COSMO_IOS_BUNDLE_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${COSMO_IOS_SHORT_VERSION}"
            MACOSX_BUNDLE_INFO_PLIST
                "${CMAKE_CURRENT_BINARY_DIR}/CosmoEnhanced-Info.plist"
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
            XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "15.0")
        return()
    endif()

    if(COSMO_VITA_APP)
        cosmo_add_episode(1)

        add_executable(cosmo_vita
            $<TARGET_OBJECTS:cosmore_game1>
            src/platform/platform_main.c
            src/launcher.c)
        target_compile_definitions(cosmo_vita PRIVATE
            COSMO_EPISODE_MODULE=1 COSMO_VITA_APP=1)
        target_link_libraries(cosmo_vita PRIVATE
            cosmo_lowlevel1 cosmo_platform cosmo_video cosmo_warnings)
        add_dependencies(cosmo_vita cosmore_game1)

        vita_create_self(eboot.bin cosmo_vita)
        set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS}" CACHE STRING "vita-mksfoex flags")
        set(VITA_PACK_VPK_FLAGS "${VITA_PACK_VPK_FLAGS}" CACHE STRING "vita-pack-vpk flags")
        separate_arguments(COSMO_VITA_MKSFOEX_ARGS NATIVE_COMMAND
            "${VITA_MKSFOEX_FLAGS}")
        separate_arguments(COSMO_VITA_PACK_VPK_ARGS NATIVE_COMMAND
            "${VITA_PACK_VPK_FLAGS}")

        set(COSMO_VITA_PARAM_SFO "${CMAKE_CURRENT_BINARY_DIR}/cosmo-vita_param.sfo")
        add_custom_command(OUTPUT "${COSMO_VITA_PARAM_SFO}"
            COMMAND "${VITA_MKSFOEX}" ${COSMO_VITA_MKSFOEX_ARGS}
                -s "APP_VER=${VITA_VERSION}" -s "TITLE_ID=${VITA_TITLEID}"
                "${VITA_APP_NAME}" "${COSMO_VITA_PARAM_SFO}"
            DEPENDS eboot.bin-self
            COMMENT "Generating param.sfo for Cosmo Enhanced"
            VERBATIM)

        set(COSMO_VITA_VPK "${CMAKE_CURRENT_BINARY_DIR}/cosmo-vita.vpk")
        set(COSMO_VITA_VPK_RESOURCES
            "${CMAKE_CURRENT_SOURCE_DIR}/vita/sce_sys/icon0.png=sce_sys/icon0.png")
        set(COSMO_VITA_VPK_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/vita/sce_sys/icon0.png")
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gamedata/COSMO1.STN" AND
           EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gamedata/COSMO1.VOL")
            list(APPEND COSMO_VITA_VPK_RESOURCES
                "${CMAKE_CURRENT_SOURCE_DIR}/gamedata/COSMO1.STN=COSMO1.STN"
                "${CMAKE_CURRENT_SOURCE_DIR}/gamedata/COSMO1.VOL=COSMO1.VOL")
            list(APPEND COSMO_VITA_VPK_DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/gamedata/COSMO1.STN"
                "${CMAKE_CURRENT_SOURCE_DIR}/gamedata/COSMO1.VOL")
        else()
            message(STATUS "Episode 1 game data not found; the VPK will need COSMO1.STN and COSMO1.VOL copied to ux0:/data/digows/Cosmo/")
        endif()
        set(COSMO_VITA_SUPPORTED_MODS
            crt-scanlines
            crt-shader
            mp3-music
            smooth-gameplay
            soft-sound-effects
            widescreen)
        foreach(COSMO_VITA_MOD IN LISTS COSMO_VITA_SUPPORTED_MODS)
            set(COSMO_VITA_MOD_DIR
                "${CMAKE_CURRENT_SOURCE_DIR}/Mods/${COSMO_VITA_MOD}")
            if(EXISTS "${COSMO_VITA_MOD_DIR}/mod.json")
                list(APPEND COSMO_VITA_VPK_RESOURCES
                    "${COSMO_VITA_MOD_DIR}=Mods/${COSMO_VITA_MOD}")
                file(GLOB_RECURSE COSMO_VITA_MOD_FILES CONFIGURE_DEPENDS
                    LIST_DIRECTORIES false "${COSMO_VITA_MOD_DIR}/*")
                list(APPEND COSMO_VITA_VPK_DEPENDS ${COSMO_VITA_MOD_FILES})
            endif()
        endforeach()
        set(COSMO_VITA_VPK_ARGS)
        foreach(COSMO_VITA_VPK_RESOURCE IN LISTS COSMO_VITA_VPK_RESOURCES)
            list(APPEND COSMO_VITA_VPK_ARGS -a "${COSMO_VITA_VPK_RESOURCE}")
        endforeach()
        add_custom_command(OUTPUT "${COSMO_VITA_VPK}"
            COMMAND "${VITA_PACK_VPK}" ${COSMO_VITA_PACK_VPK_ARGS}
                ${COSMO_VITA_VPK_ARGS}
                -s "${COSMO_VITA_PARAM_SFO}" -b "${CMAKE_CURRENT_BINARY_DIR}/eboot.bin"
                "${COSMO_VITA_VPK}"
            DEPENDS "${COSMO_VITA_PARAM_SFO}"
                "${CMAKE_CURRENT_BINARY_DIR}/eboot.bin" eboot.bin-self
                ${COSMO_VITA_VPK_DEPENDS}
            COMMENT "Building VPK cosmo-vita.vpk"
            VERBATIM)
        add_custom_target(cosmo_vita_package ALL DEPENDS "${COSMO_VITA_VPK}")
        add_dependencies(cosmo_vita_package eboot.bin-self)
        return()
    endif()

    cosmo_add_episode(1)
    cosmo_add_episode(2)
    cosmo_add_episode(3)

    # A fourth episode exists only if one has been generated; see
    # tools/make-episode.py and patches/0008.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/episodes/episode4.h")
        cosmo_add_episode(4)
        set(COSMO_EPISODE4 ON)
    endif()

    # The front door: finds which episodes have data present and runs one.
    if(COSMO_BUNDLE AND WIN32)
        # An icon, and no console window behind the game.
        add_executable(cosmo WIN32 src/launcher.c "${COSMO_WIN_ICON_RC}")
    else()
        add_executable(cosmo src/launcher.c)
    endif()
    target_link_libraries(cosmo PRIVATE cosmo_video cosmo_platform cosmo_warnings)
    if(WIN32)
        add_dependencies(cosmo cosmo1_module cosmo2_module cosmo3_module)
    else()
        add_dependencies(cosmo cosmo1 cosmo2 cosmo3)
    endif()

    if(COSMO_EPISODE4)
        target_compile_definitions(cosmo PRIVATE COSMO_HAVE_EPISODE4)
        if(WIN32)
            add_dependencies(cosmo cosmo4_module)
        else()
            add_dependencies(cosmo cosmo4)
        endif()
    endif()

    # ----------------------------------------------------------------------
    # macOS application bundle
    # ----------------------------------------------------------------------
    #
    # The launcher is the bundle's program. The episode programs sit beside it
    # in Contents/MacOS, which is where SDL_GetBasePath() points, so the
    # launcher finds them the same way it does anywhere else.
    #
    # The game data is deliberately not placed here. It belongs to Apogee and
    # is not in this repository; whoever assembles a release drops it into
    # Contents/Resources, and paths.c looks there.
    if(COSMO_BUNDLE AND APPLE)
        set_target_properties(cosmo PROPERTIES
            MACOSX_BUNDLE TRUE
            OUTPUT_NAME "Cosmo"
            MACOSX_BUNDLE_BUNDLE_NAME "Cosmo"
            MACOSX_BUNDLE_GUI_IDENTIFIER "com.digows.cosmo"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
            MACOSX_BUNDLE_ICON_FILE "icon"
            MACOSX_BUNDLE_COPYRIGHT
                "Game (c) 1992 Apogee Software. Port under the MIT licence.")

        set(_bundle "$<TARGET_BUNDLE_CONTENT_DIR:cosmo>")

        add_custom_command(TARGET cosmo POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_bundle}/Resources"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${COSMO_ICON_DIR}/icon.icns" "${_bundle}/Resources/icon.icns"
            COMMENT "Adding the icon to Cosmo.app")

        foreach(_ep 1 2 3)
            add_custom_command(TARGET cosmo POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "$<TARGET_FILE:cosmo${_ep}>" "${_bundle}/MacOS/"
                COMMENT "Adding episode ${_ep} to Cosmo.app")
        endforeach()

        if(COSMO_EPISODE4)
            add_custom_command(TARGET cosmo POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "$<TARGET_FILE:cosmo4>" "${_bundle}/MacOS/")
        endif()
    endif()
endif()

# --------------------------------------------------------------------------
# Tools
# --------------------------------------------------------------------------

if(NOT COSMO_VITA_APP)
    add_executable(imgview tools/imgview.c)
    target_link_libraries(imgview PRIVATE cosmo_video cosmo_warnings)
endif()

# --------------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------------

if(COSMO_BUILD_TESTS)
    enable_testing()
    add_executable(test_ega tests/test_ega.c)
    target_link_libraries(test_ega PRIVATE cosmo_core cosmo_warnings)
    add_test(NAME ega COMMAND test_ega)

    add_executable(test_mods tests/test_mods.c)
    target_link_libraries(test_mods PRIVATE cosmo_video cosmo_warnings)
    add_test(NAME mods COMMAND test_mods)
endif()
