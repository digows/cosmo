# Cross-compile a Windows build with MinGW-w64.
#
# Used to check the Windows target from a Linux machine or container without a
# Windows box in the loop:
#
#   cmake -S . -B build/win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake
#
# SDL3 is built from source here, since there is no system copy to find.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)

set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Nothing to find on the host, so go straight to building SDL.
set(COSMO_FETCH_SDL ON CACHE BOOL "" FORCE)
