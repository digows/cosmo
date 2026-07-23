**Cosmo's Cosmic Adventure: Forbidden Planet** (Apogee, 1992), running
natively. No DOSBox, no emulator.

Every download includes the shareware Episode 1 and plays as soon as it is
unpacked. On macOS it is an application you double-click; on Windows and Linux,
a folder with the game in it.

If you own Episodes 2 and 3, choose them in the launcher and it will ask you to
find your copy.

| Download | For |
|---|---|
| `cosmo-*-macos.zip` | macOS 11 or newer, Apple Silicon and Intel |
| `cosmo-*-windows.zip` | Windows, 64-bit |
| `cosmo-*-linux.tar.gz` | Linux, x86-64 |

macOS refuses to open this the first time, because it is not signed by a paid
Apple developer account. To allow it, open **System Settings → Privacy &
Security**, scroll to Security, and click **Open Anyway** next to the message
about Cosmo. On macOS 15 and later that panel is the only route — right-clicking
and choosing Open no longer works. From a terminal,
`xattr -dr com.apple.quarantine /path/to/Cosmo.app` does the same in one step.

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
release. Episodes 2 and 3 are not included and are still sold.

This port's code is MIT. Cosmore is MIT, © Scott Smitelli and contributors;
ymfm is BSD 3-Clause, © Aaron Giles. Full credits are in
[ATTRIBUTION.md](https://github.com/digows/cosmo/blob/main/ATTRIBUTION.md), and
the story of what it took is in
[docs/porting-notes.md](https://github.com/digows/cosmo/blob/main/docs/porting-notes.md).
