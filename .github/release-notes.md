## What's new in v0.4.0

- **Widescreen** adds a 16:9 playfield and a full-width HUD. Enable it in
  **Mods**. CRT and scanlines work in widescreen and in the original view.
- The launcher uses Cosmo's original dialog frames and marks missing episodes
  as **Not found**. Select a missing episode to locate its files.
- The scrolling **Mods** menu includes Character Colour with a live preview,
  Gamepad Rumble, and separate choices for Filter, Soundtrack and SFX.
- In-game **Game Settings** has working Keyboard Mapping, a Gamepad Controls
  guide, music and sound volume controls, and the same Filter setting as Mods.
- Music fades when the pause menu opens and returns when you resume. Save and
  restore now check whether their file operations succeeded.

The Windows download includes the shareware Episode 1 and plays as soon as it
is unpacked.

If you own Episodes 2 and 3, choose them in the launcher and it will ask you to
find your copy.

| Download | For |
|---|---|
| `cosmo-*-windows.zip` | Windows, 64-bit |

Arrow keys move, **space** jumps, **alt** bombs, **F1** helps, **Q** then **Y**
quits and saves. All of it is rebindable from the game's own menu. `PLAYING.txt`
in the archive has the rest.

## What this is

Built from the game's own code rather than a reimplementation, by way of
[Cosmore](https://github.com/smitelli/cosmore) — Scott Smitelli's
reconstruction of the 1992 source — with only the layer that talked to PC
hardware replaced. The physics, the actors, the collision handling and the bugs
are Todd Replogle's. What is new underneath is an emulated EGA adapter, the
programmable interval timer, the keyboard controller, the PC speaker and an
AdLib.

`SHA256SUMS.txt` covers every archive.

## Credits

*Cosmo's Cosmic Adventure*, its artwork and its music are © 1992 Apogee
Software, Ltd. Episode 1 is included as the freely distributable shareware
release. Episodes 2 and 3 are not included and are still sold. The optional mod
audio is derived from Episode 1's shareware data.

This port's code is MIT. Cosmore is MIT, © Scott Smitelli and contributors;
ymfm is BSD 3-Clause, © Aaron Giles. Full credits are in
[ATTRIBUTION.md](https://github.com/digows/cosmo/blob/main/ATTRIBUTION.md), and
the story of what it took is in
[docs/porting-notes.md](https://github.com/digows/cosmo/blob/main/docs/porting-notes.md).
