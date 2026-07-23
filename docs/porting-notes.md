# Porting notes

What actually broke when 1992 DOS code met a modern machine, how each thing was
found, and how it was checked. Roughly in the order it happened.

Everything here is reproducible: the patches are in [`patches/`](../patches/),
the input scripts in [`tests/scripts/`](../tests/scripts/), and the environment
variables that produce the evidence are documented in the
[README](../README.md).

---

## The build system lies before the code does

**Seven patches applied cleanly and changed nothing.** `git apply` resolves a
patch's paths against whatever repository it discovers, not against the working
directory. The build tree sits inside this repository, so every path landed
outside what git considered in scope, git ignored them all — and exited 0. The
configure step reported seven successes and produced completely unpatched
sources.

This is the most dangerous kind of failure, because it looks like success. The
fix is two parts: the generated sources get a git repository of their own, so
the upward search stops there on every platform; and each patch is fingerprinted
before and after, so a patch that reports success while changing nothing halts
the build.

**Line endings pull in two directions at once.** CMake's `file(READ)` strips
carriage returns — measured: 22 bytes in, 20 bytes out — so the generated
sources come out LF on Unix. On Windows `file(WRITE)` puts them back. Git,
meanwhile, hands over the patch files as CRLF on a Windows checkout regardless
of what `.gitattributes` asks for. There is no single side to fix, so the patches
are applied with `--ignore-whitespace`, verified against all four combinations
of LF and CRLF on either side.

---

## The 1992 code

These are the [numbered patches](../patches/). Each one has a preamble
explaining the defect; patch 0007 states plainly that it is the one deliberate
change rather than a fix.

### Interrupts masked forever

`game2.c` contains three calls to `disable()` and none to `enable()`. In the
original, each was paired with a `pushf` / `popf` around the critical section,
and the assembly is what restored the flag. Commenting the assembly out left the
`disable()` with nothing to undo it.

`SetAdLibRegister()` is called several hundred times while probing for an AdLib,
so the first one masked interrupts for the rest of the process's life. The timer
interrupt never landed and every `WaitHard()` spun forever.

The same discovery showed that `disable()`/`enable()` must be a flag rather than
a nesting counter — the 8086 interrupt flag is a single bit, and the game relies
on that.

### The compiler optimises away the game clock

`WaitHard()` is the clearest case:

```c
gameTickCount = 0;
while (gameTickCount < delay)
    ;  /* VOID */
```

On a 286 this worked because the handler ran on the same processor and Turbo C
2.0 did not optimise hard enough to notice. Here the handler runs on the main
thread while the game runs on its own, and any modern compiler at `-O2` hoists
the load out of the loop. Every global shared with an interrupt handler is now
`volatile`, which restores an assumption the original was entitled to make.

### A null pointer that used to be harmless

`PCSpeakerService()` dereferences `soundDataPtr[activeSoundIndex]` without
checking it, and runs for several hundred timer ticks before `Startup()` loads
the sound data. In real mode the null far pointer aimed at `0000:0000` — the
interrupt vector table — which is readable memory holding values that were never
going to equal `END_SOUND`. The read returned junk and the game carried on. With
a modern memory map the same read faults on the first tick.

### Every note went to register zero

The music engine reads 16-bit chunks and three assembly instructions split each
one into an OPL2 register address and a value. Without them both variables were
uninitialised, so every music event became a write of 0 to register 0.

The engine had been running correctly the whole time, at the right tempo,
delivering every note to the wrong place. Found by logging the register writes
(`COSMO_OPL_LOG=1`) and seeing `opl 00=00` 798 times in a row.

This prompted an audit of every remaining line the assembly transformation had
commented out. One more was load-bearing: `TimerInterruptService()` accumulated
the PIT divisor into a 16-bit counter and chained to the BIOS handler only on
carry, which is how the system clock keeps its own 18.2 Hz while the game runs
the timer at 140 or 560.

### The microsecond wait that stopped waiting

`ProfileCPU()` and `WaitWallclock()` were built as a pair: the first counted how
many iterations of a tight assembly loop fit between two timer interrupts, the
second then busy-waited by running that loop. Both loops were assembly, so
calibration read a variable nothing wrote and the wait returned immediately.

AdLib detection starts an OPL2 timer, waits 100 microseconds, and checks whether
the timer's flag came on — so with no wait, the chip could never be found and
the game stayed silent. The calibrated values become the microsecond counts they
always stood for, and the wait is done by the clock.

---

## Things the platform layer had to get right

Not patches — these are places where a plausible-looking implementation is
quietly wrong.

### `getw` moves a different number of bytes

Borland's `getw`/`putw` move a 16-bit word. The POSIX functions of the same name
move an `int`, which is 32 bits here. Every map header was read at double width
and at the wrong offsets, so `mapWidth` never matched any case in the switch
that derives `mapYPower`, and `DrawMapRegion()` drew a single row of tiles
instead of a screen.

The game was running correctly the whole time — 10.8 frames per second, exactly
the thirteen timer ticks it asks for — it just had almost nothing to draw. The
same fix repaired save files.

### The palette is not what it looks like

Mode 0Dh is a 200-line mode, so only four bits of the six-bit colour value reach
the screen, and they are not contiguous: bit 0 blue, bit 1 green, bit 2 red,
bit 4 intensity. The 64 possible values are really 16 colours repeated four
times. The monitor also special-cased RGBI 1100 into brown rather than a dark
yellow.

Reading the value as the full six-bit `r'g'b'RGB` form still renders the BIOS
default palette convincingly, which is why this survived until the game
installed its own palette during a fade-in and the title screen came out green.
Now covered by unit tests.

### Port 0x60 is never zero on a real machine

`IsAnyKeyDown()` tests the keyboard data port directly for a clear high bit.
Starting that port at zero reads as a key being held, so the title screen fell
straight through its idle loop into a wait that never returned. A real machine
would have latched a release code before the game started.

### The window system takes the controls

macOS binds Control with every arrow key to Mission Control, so `Ctrl` plus a
direction — jump while moving — never reaches the application at all.

Diagnosed rather than guessed: scripted input showed the game's own `cmdJump`
and `cmdWest` set simultaneously, which proved the emulation correct and moved
the search to the window system. Command now reports the same scancode, and the
shipped default moved to space.

macOS also suppresses key-up events for other keys while Command is held, and
drops them entirely when the window loses focus — either would leave the game
believing a key is still down. Held keys are reconciled against SDL's own view
once a frame.

---

## How things were checked

Claims in this project are meant to be backed by measurement rather than by the
code looking right.

| Claim | How it was checked |
|---|---|
| Sound effects are correct | Captured audio compared against the PIT divisors decoded from the game's own `SOUNDS.MNI`. `SND_NEW_GAME` is 284, 385, 477, 568, 746 Hz in the data; the capture measures 281, 382, 484, 562, 741 Hz |
| Music is real music | Strongest partials of the title theme at 194, 388, 486 and 583 Hz — G, G an octave up, B and D, a G major triad. Levels peak at 41% of full scale with no clipped samples across a minute |
| Jump composes with movement | Scripted input plus a readout of the game's own key and command state, showing both set on the same frame |
| The emulated EGA behaves | Unit tests for map mask, bit mask, write modes 0/1/2, set/reset, latch loading and the palette, none of which need game data |
| Timing matches the original | The game programs 140 Hz and gets 140 Hz; the attract sequence reaches its credits screen twelve seconds after the title, against the 12.86 seconds the code asks for |
| Linux and Windows build | A container matching the CI runner, and a MinGW cross-compile, both run before pushing. Recipes in [`tools/checks/`](../tools/checks/) |

That last row is worth its own note. Running the Linux job locally found a
missing `libxtst-dev` that would have failed CI at configure; the MinGW
cross-compile found that `_execv` takes a const argument vector on Windows and a
non-const one on POSIX. Both were fixed before any runner time was spent.
