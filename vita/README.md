# PlayStation Vita homebrew prototype

This experimental target builds an Episode 1-only VPK with SDL3's Vita
backend. It requires VitaSDK and a PS Vita capable of running homebrew. SDL3
is built from source for the Vita toolchain.

## Build

Initialize the source submodules, install VitaSDK, and install host CMake 3.21+
and Ninja. Set `VITASDK` to the SDK installation directory and ensure its
`bin` directory is on `PATH`. From the repository root:

```sh
cmake -S . -B build/vita -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DCOSMO_VITA_APP=ON \
  -DCOSMO_FETCH_SDL=ON \
  -DCOSMO_BUILD_TESTS=OFF
cmake --build build/vita
```

The package is `build/vita/cosmo-vita.vpk`. On Windows, run the commands from
the VitaSDK MSYS2 shell or WSL with the SDK installed in that environment.

If `gamedata/COSMO1.STN` and `gamedata/COSMO1.VOL` are present at configure
time, CMake includes them in the VPK. Game data is not distributed with this
source. Otherwise copy your own Episode 1 files to
`ux0:/data/digows/Cosmo/` after installing the VPK.

The VPK includes the bundled CRT, MP3 soundtrack, smooth-gameplay,
soft-sound-effects, and widescreen mods. MP3 tracks are decoded with the Vita
SDK's audio decoder; original IMF music and bundled WAV sound-effect
replacements are also supported.

The prototype starts with the existing episode launcher and can play Episode
1. Use the Vita D-pad or left stick to move, Cross to jump, Circle to throw a
bomb or go back, and Start to open the in-game menu. Episode import and
Episodes 2/3 are not part of this first target.
