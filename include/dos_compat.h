/*
 * dos_compat.h -- Camada de compatibilidade Borland/DOS -> macOS/clang.
 *
 * Force-incluido (-include) antes de qualquer fonte do Cosmore. Substitui os
 * headers da Borland (alloc.h, conio.h, dos.h, mem.h) que o prep.sh remove.
 *
 * Fase 1: objetivo e SO compilar. As funcoes sao declaracoes; a implementacao
 * real (EGA emulado, PIT, OPL2, teclado) vem na camada de plataforma.
 */

#ifndef DOS_COMPAT_H
#define DOS_COMPAT_H

#include <stdint.h>
#include <stdlib.h>

/* A palavra-chave `interrupt` da Borland nao existe fora do x86 real mode. */
#define interrupt

/* Modo de texto 80x25 colorido (conio.h) */
#define C80 3

/*
 * MK_FP: no DOS montava um ponteiro far seg:ofs. Aqui mapeia para os buffers
 * emulados. Stub por enquanto -- a Fase 2 troca por lookup na VRAM emulada.
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

/* Pseudo-registradores da Borland: variaveis globais na camada de plataforma */
extern unsigned int _AX, _BX, _CX, _DX;
extern unsigned char _AH, _AL, _BH, _BL, _CH, _CL, _DH, _DL;

#endif /* DOS_COMPAT_H */
