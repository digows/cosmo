# Widescreen

In the main launcher, open **Customise**, turn **Widescreen** on, and start an
episode. The setting is saved alongside your other mod preferences. It draws
more of the level and uses a full-width HUD.
Narrow maps and demos keep their original playfield and centered HUD.
The **Filter** setting applies to the full widescreen frame, including the HUD.

The separate Windows preview lives in `build/windows-wide` and uses its own
`episodes` directory for game data, saves and settings. Open its `cosmo.exe`
to play. The main build in `build/windows-mingw` defaults to the original 4:3
mode until you enable the mod. The preview build defaults to widescreen.

## Milestone 1 � 16:9 terrain preview

Use a 432x243 presentation surface. Preserve the original EGA pixel aspect
ratio and centre the title screens. In gameplay, draw up to 14 extra columns
of level terrain and parallax backdrop to the right of the original play area.
The original HUD is drawn below the playfield. The first preview disabled CRT
blur and scanlines while the widescreen renderer was being tested.

## Milestone 2 � Widen scene rendering

The extra 14 columns now use the same world draw order as the original
playfield. Moving platforms update the map before terrain is drawn.
Sprites, fountains, shards, explosions, rain and lights reach the extension.
Unactivated actors are visible in their resting pose until they enter the
gameplay activation area.

## Milestone 3 � Tune camera and gameplay

For maps at least 52 tiles wide, stop the camera when the complete viewport
reaches the right edge. Actor activation keeps the original 38-column width
and shifts towards Cosmo when he moves beyond the original camera's right
side. Narrow maps and attract-mode demos use the original viewport.
Sprite collision checks handle movement above or below the tile map without
reading outside the tile buffer.

Remaining play checks include timing and feel near both map edges,
transporter and boss sequences, and runtime checks with the Episode 2
and Episode 3 data.

## Rebuild the prototype locally

Run from the source root with the same MSYS2 MinGW-w64 tools as the regular
Windows build.

```powershell
cmake -S . -B build/windows-wide -G Ninja -DCOSMO_WIDESCREEN=ON -DCOSMO_BUNDLE=ON
cmake --build build/windows-wide
ctest --test-dir build/windows-wide --output-on-failure
```

The prototype needs local episode data in `build/windows-wide/episodes`,
and the SDL runtime DLLs beside `cosmo.exe` when using the shared SDL build.
