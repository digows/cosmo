# Attribution and provenance

## Cosmo's Cosmic Adventure: Forbidden Planet

© 1992 Apogee Software, Ltd. Released for MS-DOS in March 1992.

- Programmer: Todd J. Replogle
- Graphic artist: Stephen A. Hornback
- Producer: Scott Miller
- Level design: Todd J. Replogle
- Level editor: Allen H. Blum III
- AdLib music: Bobby Prince
- Music routines: id Software

All rights to the game, its assets and its trademarks belong to Apogee
Software. No game asset is redistributed by this repository.

The game is still sold on Steam, GOG and Zoom Platform. Episode 1 was
distributed as shareware and is freely redistributable.

## Cosmore

<https://github.com/smitelli/cosmore> — © 2020-2024 Scott Smitelli and
contributors, MIT licensed.

A reconstruction of the v1.20 source code, recovered by disassembling the
original executables and buildable with Turbo C 2.0 and TASM 1.0. It took
close to five years and reaches 96.3% byte-for-byte agreement with the 1992
binaries. The remainder is uninitialized data segment addresses, whose original
layout cannot be recovered from a binary — the code itself matches.

Pinned commit: `80418d1ee9026594e4dcc8445ec801d374074d84` (2024-10-23).

Beyond the source, this project uses the pure C implementation of the drawing
routines published in `C-DRAWING.md`, and the porting notes in
`MODERN-COMPILERS.md`.

## Cosmodoc

<https://cosmodoc.org/> — roughly 262,000 words documenting the internals of
the game, also by Scott Smitelli. The reference for the file formats and for
the hardware behaviour emulated here.
