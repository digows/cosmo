# Mod support

On Windows, put mods in `Mods/` beside `cosmo.exe`. On Android, bundled mods
are copied to the app's private files directory and can be toggled in the
launcher. To bundle your own mods in an Android build, add them to `Mods/` in
the source tree before building the APK. Each mod lives in its own folder
and has a UTF-8 `mod.json` file.

```json
{
  "id": "example-mod",
  "name": "Example Mod",
  "version": "1.0.0",
  "author": "Example Author",
  "description": "An example replacement for Cosmo.",
  "enabled": true
}
```

`id`, `name`, and `version` are required. `author`, `description`, and
`enabled` are optional; preferences in `Settings/mods.json` take precedence
over `enabled`. The optional `manifestVersion` is currently `1`. The settings
file also records known IDs so a newly installed mod can use its manifest
default without re-enabling one the player previously disabled. Invalid JSON,
missing required values, unsupported manifest versions, inaccessible files,
and duplicate IDs are logged and skipped. A folder's name may differ from its
ID.

Open **Customise** from the launcher. Use Up/Down to browse, Left/Right to
change **Filter**, **Soundtrack** or **SFX**, and Enter or Space to toggle
individual mods. On Android, use the arrow buttons, **Select**, and **Back**.
Escape returns to the episode list on a keyboard. The launcher and episode
programs share the same preference file. It is stored under
`Settings/mods.json` in the writable data/save directory; when the install
folder is read-only, that directory is the user's Cosmo application-data
folder.

## Replacing graphics

The first supported override is a raw extracted asset file. For example:

```text
Mods/example-mod/
  mod.json
  graphics/
    PRETITLE.MNI
    episode1/
      TITLE1.MNI
```

Files must use the original asset's exact format, byte layout, and byte length. The existing
assets are entries in Apogee's `COSMOn.STN` and `COSMOn.VOL` group files; `.MNI`
images use the game's EGA planar image data. The mod loader reads an extracted
replacement for an asset the game already requests. It does not add new asset
names, decode new formats, or load levels and scripts.

Episode-specific `graphics/episodeN/NAME` files take precedence over a mod's
shared `graphics/NAME` file. Mods are considered in case-insensitive
alphabetical order by folder name; the first enabled mod with a matching file
wins. If no replacement is found, the original STN file is tried before VOL,
as before. Overrides are read from disk when requested, so editing a mod file
does not require a special pack step.

## Player skins

Add a `playerPalette` array of 16 integers from 0 to 15 to a mod's manifest.
Each entry replaces the sprite colour at the same index. For example, this map
gives Cosmo a blue body and cyan accents:

```json
"playerPalette": [0, 1, 1, 3, 3, 5, 6, 7, 8, 9, 9, 11, 11, 13, 14, 15]
```

The remap applies to opaque pixels in `PLAYERS.MNI` after any raw asset
replacement is loaded. Keep one skin enabled at a time. If several are
enabled, the first skin in alphabetical folder order wins. The Character Colour
picker under **Customise** overrides installed skin colours until you select
**Reset colours**.
A palette effect such as Aurora can still tint the final picture.

## MP3 music replacements

On Windows, macOS, and Android, an enabled mod can replace a track with an MP3. Put the file in
the mod's `music/` folder and use the original IMF track's basename with an
`.mp3` extension. For example, `music/mzztop.mp3` replaces `mzztop.mni`:

```text
Mods/soft-soundtrack/
  mod.json
  music/
    mzztop.mp3
```

The manifest needs the usual required fields, such as `id`, `name`, and
`version`. Enable the mod in the launcher's **Customise** menu.

The game decodes the replacement when that track starts and loops it until the
game switches tracks. The in-game Music toggle also stops and resumes it. If
the MP3 is missing or cannot be decoded, the original synthesized IMF track
plays instead. Windows uses Media Foundation for MP3 decoding, macOS uses
AudioToolbox, and Android uses MediaCodec.

## Sound effect replacements

An enabled mod can also replace individual PC speaker effects with short WAV
or MP3 clips. WAV files are checked first when both formats are present. Put
each clip in `sounds/` using the effect name. For example:

```text
Mods/soft-soundtrack/
  mod.json
  sounds/
    player_jump.wav
    prize.wav
```

Available names are `big_prize`, `player_jump`, `player_land`, `player_cling`,
`player_hit_head`, `player_pounce`, `player_death`, `door_unlock`,
`spikes_move`, `explosion`, `win_level`, `barrel_destroy_1`, `prize`,
`player_hurt`, `foot_switch_move`, `foot_switch_on`, `roamer_gift`,
`destroy_satellite`, `player_footstep`, `push_player`, `drip`,
`pipe_corner_hit`, `transporter_on`, `scooter_putt`, `destroy_solid`,
`projectile_launch`, `big_object_hit`, `no_bombs`, `place_bomb`,
`hint_dialog_alert`, `red_jumper_jump`, `red_jumper_land`,
`bghost_egg_crack`, `bghost_egg_hatch`, `saw_blade_move`, `fireball_launch`,
`object_hit`, `exit_monster_open`, `exit_monster_ingest`, `bear_trap_close`,
`pause_game`, `weeeeeeee`, `jump_pad_robot`, `text_typewriter`, `bonus_stage`,
`shard_bounce`, `tulip_launch`, `new_game`, `rocket_burn`, `smash`,
`high_score_display`, `high_score_set`, `ivy_plant_rise`, `flame_pulse`,
`boss_damage`, `boss_move`, `speech_bubble`, `baby_ghost_jump`,
`baby_ghost_land`, `thunder`, `barrel_destroy_2`, `tulip_ingest`,
`plant_mouth_open`, `entering_level_num`, and `boss_launch`.

A WAV or MP3 effect takes the place of its original PC speaker sound and mixes
with the music. WAV files must be mono 16-bit PCM; other sample rates are
resampled automatically. Missing or unreadable clips fall back to the original
effect. The bundled Soft Sound Effects mod is selected with **Customise → SFX → Softened**.

## Bundled gameplay mods

Enable these bundled mods in **Customise**. Each mod's manifest uses the
field shown below.

| Mod | Manifest field | Effect |
|---|---|---|
| Widescreen | `widescreen` | Enables a wider playfield and HUD on Windows and macOS. The Android release already uses widescreen gameplay. |
| Smooth Gameplay | `smoothGameplay` | Smooths scrolling, character movement, and animations. |

## CRT shader example

The renderer is a software EGA-to-RGB pipeline, so this port does not load
arbitrary GPU shader files. A mod can request the built-in CRT-style post
process with `"crtShader": true` in its manifest. This enables phosphor blur
and glow:

```json
{
  "id": "crt-shader",
  "name": "CRT Shader",
  "version": "1.0.0",
  "author": "Cosmo mod example",
  "description": "Adds soft phosphor blur and glow.",
  "enabled": true,
  "crtShader": true
}
```

The release already includes the CRT and Scanlines mods. Choose **CRT**,
**Scanlines**, **Both**, or **None** under **Customise → Filter**.

Scanlines are a separate effect. Put `"crtScanlines": true` in another mod's
manifest to enable them independently, or enable both mods together:

```json
{
  "id": "crt-scanlines",
  "name": "CRT Scanlines",
  "version": "1.0.0",
  "enabled": true,
  "crtScanlines": true
}
```

## Windows build

The Windows release uses MSYS2's UCRT64 environment with GCC, CMake, Ninja,
and Git. After cloning the repository with its submodules, run from the source
root in the UCRT64 shell.

```text
cmake -S . -B build -G Ninja -DCOSMO_FETCH_SDL=ON -DCOSMO_BUNDLE=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## macOS build

Install CMake and SDL3 (or pass `-DCOSMO_FETCH_SDL=ON` to build SDL3 from source),
then build the application bundle from the source root:

```text
cmake -S . -B build -DCOSMO_BUNDLE=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

See the [game data guide](../gamedata/README.md) for the files needed to play
a source build.
