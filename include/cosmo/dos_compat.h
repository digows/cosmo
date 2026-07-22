/*
 * dos_compat.h -- Borland/DOS compatibility shim.
 *
 * Force-included ahead of every Cosmore source, replacing the Borland headers
 * (alloc.h, conio.h, dos.h, io.h, mem.h) that the source preparation step
 * strips out.
 *
 * These are declarations only. The implementations live in the platform layer:
 * port I/O is routed to the emulated EGA, PIT, PIC and OPL2, and the interrupt
 * vectors become scheduled callbacks.
 */

#ifndef COSMO_DOS_COMPAT_H
#define COSMO_DOS_COMPAT_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Borland's `interrupt` calling convention does not exist outside real mode. */
#define interrupt

/* conio.h: 80x25 color text mode */
#define C80 3

/*
 * MK_FP built a far pointer from a segment and offset. Here it resolves to the
 * emulated memory the segment stands for.
 */
void *dos_map_segment(unsigned seg, unsigned ofs);
#define MK_FP(seg, ofs) dos_map_segment((unsigned)(seg), (unsigned)(ofs))

/*
 * Video memory cannot be reached through MK_FP: a plain `*ptr = value` store
 * would bypass the plane, latch and bit mask logic of the adapter. The two
 * call sites in game1.c that did that are rerouted to these by patches/0001.
 */
void dos_vram_write(unsigned offset, unsigned char value);
unsigned char dos_vram_read(unsigned offset);
#define EGA_WRITE(offset, value) \
    dos_vram_write((unsigned)(offset), (unsigned char)(value))
#define EGA_READ(offset) dos_vram_read((unsigned)(offset))

/*
 * Stand-ins for the `pushf` / `popf` pairs that bracket critical sections in
 * game2.c. They carry the interrupt flag, which is the only one the code
 * around them depends on. See patches/0003.
 */
unsigned long SaveInterruptFlag(void);
void RestoreInterruptFlag(unsigned long saved);

/* Called when the game flips display pages; drives presentation. */
void platform_page_flipped(void);

#define random(num) (rand() % (num))

enum COLORS {
    BLACK, BLUE, GREEN, CYAN, RED, MAGENTA, BROWN, LIGHTGRAY, DARKGRAY,
    LIGHTBLUE, LIGHTGREEN, LIGHTCYAN, LIGHTRED, LIGHTMAGENTA, YELLOW, WHITE
};

union REGS {
    struct {unsigned int ax, bx, cx, dx, si, di, cflag, flags;} x;
    struct {unsigned char al, ah, bl, bh, cl, ch, dl, dh;} h;
};

/*
 * Borland's getw/putw move a 16-bit word. The POSIX functions of the same name
 * move an `int`, which is 32 bits here -- so every map header and save file
 * would be read at double width and at the wrong offsets. These replace them.
 *
 * The signatures match the POSIX declarations so that the macro rename below
 * stays compatible with whatever <stdio.h> declares.
 */
int cosmo_getw(FILE *fp);
int cosmo_putw(int value, FILE *fp);
#define getw cosmo_getw
#define putw cosmo_putw

unsigned long coreleft(void);
void disable(void);
void enable(void);
long filelength(int handle);
int getch(void);
void (*getvect(int interruptno))(void);
unsigned char inportb(int portid);
int int86(int intno, union REGS *inregs, union REGS *outregs);
void movmem(void *src, void *dest, unsigned length);
void outport(int portid, int value);
void outportb(int portid, unsigned char value);
void setvect(int interruptno, void (*isr)(void));
void textmode(int newmode);
char *ultoa(unsigned long value, char *string, int radix);
char *strupr(char *string);
void geninterrupt(int intno);

/* Borland's pseudo-registers, backed by globals in the platform layer. */
extern unsigned int _AX, _BX, _CX, _DX;
extern unsigned char _AH, _AL, _BH, _BL, _CH, _CL, _DH, _DL;

#endif /* COSMO_DOS_COMPAT_H */
