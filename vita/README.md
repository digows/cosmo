# PlayStation Vita port

The Vita port runs Episode 1 using SDL3 and GXM. Install the VPK through
VitaShell on a homebrew-enabled Vita. Building from source requires VitaSDK.

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
SDK's native MP3 decoder on a background thread. Original IMF music remains
available. The launcher preloads the WAV sound effects, which can play on up
to four voices.

Use the D-pad or left stick to move, Cross to jump or confirm, Circle to throw
a bomb, and Start to open the in-game menu. SELECT or L goes back or opens the
quit prompt in gameplay. Qualifying high scores use the Vita's on-screen
keyboard. Episode import and Episodes 2 and 3 are unavailable on Vita.

Set `COSMO_VITA_AUDIO_DEBUG=ON` for an instrumented build with a separate
LiveArea bubble. It logs renderer and audio timings. To show its overlay,
create `ux0:data/digows/Cosmo/graphics-overlay` before launching it.
