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

#define random(num) (rand() % (num))

enum COLORS {
    BLACK, BLUE, GREEN, CYAN, RED, MAGENTA, BROWN, LIGHTGRAY, DARKGRAY,
    LIGHTBLUE, LIGHTGREEN, LIGHTCYAN, LIGHTRED, LIGHTMAGENTA, YELLOW, WHITE
};

union REGS {
    struct {unsigned int ax, bx, cx, dx, si, di, cflag, flags;} x;
    struct {unsigned char al, ah, bl, bh, cl, ch, dl, dh;} h;
};

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
