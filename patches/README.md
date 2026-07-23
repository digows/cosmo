# Patches

Changes to the reconstructed 1992 sources in
[`vendor/cosmore`](https://github.com/smitelli/cosmore), applied at configure
time by [`cmake/PrepareCosmoreSources.cmake`](../cmake/PrepareCosmoreSources.cmake).
The submodule itself is never modified, so the difference between the original
and what is compiled is exactly this directory plus three mechanical
transformations.

Everything the transformations can express is done there rather than here:
dropping Borland headers with no modern equivalent, commenting out the 16-bit
inline assembly, and pinning the base types to their original widths. A patch
exists only where that is not enough.

Each patch carries a preamble explaining the defect and why the change is
correct. Six restore behaviour the port would otherwise have lost; one is a
deliberate change and says so.

| Patch | What it addresses |
|---|---|
| [0001](0001-route-direct-video-memory-writes-through-the-emulated-ega.patch) | Two places store through a far pointer into video memory, which cannot be intercepted, so they are routed through the emulated adapter |
| [0002](0002-mark-interrupt-shared-globals-volatile.patch) | Globals shared with interrupt handlers become `volatile`; without it a modern compiler hoists the game clock out of its wait loop and the game hangs |
| [0003](0003-restore-interrupt-flag-save-restore-and-opl2-writes.patch) | `pushf`/`popf` around critical sections, and the OPL2 register writes that lived inside the assembly. Without this `game2.c` has three `disable()` calls and no `enable()` |
| [0004](0004-guard-the-sound-pointer-before-it-is-loaded.patch) | A null sound pointer that real mode made harmless by aiming it at the interrupt vector table |
| [0005](0005-restore-microsecond-waits-lost-with-the-assembly.patch) | The calibrated busy-wait pair that AdLib detection depends on, replaced by a real microsecond wait |
| [0006](0006-restore-the-two-remaining-assembly-blocks-that-did-real-work.patch) | The music chunk split, without which every note is written to register 0, and the BIOS timer chaining |
| [0007](0007-default-to-space-for-jump.patch) | **Not a fix.** Changes the default jump key from ctrl to space |

[`docs/porting-notes.md`](../docs/porting-notes.md) has the longer story of how
each was found.

## Regenerating a patch

Patches apply to the *generated* sources, after the transformations, not to the
submodule. To revise one, configure a build, edit a copy of the file in
`build/<preset>/gen/`, and diff it against the unmodified generated version.

The build fingerprints every generated file before and after each patch, so a
patch that applies cleanly while changing nothing fails the configure step
rather than silently doing nothing.
