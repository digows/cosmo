/*
 * interrupts.c -- Interrupt vectors and BIOS services.
 *
 * The game installs its own handlers for int 8 (the timer, which paces the
 * whole game clock) and int 9 (the keyboard). Here the vector table is
 * ordinary storage and the main thread plays the part of the interrupt
 * controller, calling handlers at the rate the PIT was programmed for.
 *
 * disable()/enable() become a real mask. When the game masks interrupts to
 * install a vector, delivery genuinely waits -- which is the point of the
 * instruction, and skipping it would introduce races the original never had.
 */

#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "cosmo/dos_compat.h"
#include "cosmo/ega.h"
#include "cosmo/hardware.h"

#define VECTOR_COUNT 256

static interrupt_handler vectors[VECTOR_COUNT];
static SDL_Mutex *hw_mutex;

/*
 * The 8086 interrupt flag is a single bit, so this is a flag and not a nesting
 * counter. That distinction matters: the game calls disable() several times
 * without a matching enable(), relying on a later `popf` or `sti` to clear it.
 * Counting would leave interrupts masked forever.
 */
static bool interrupts_masked;

void hardware_lock(void)
{
    if (hw_mutex) SDL_LockMutex(hw_mutex);
}

void hardware_unlock(void)
{
    if (hw_mutex) SDL_UnlockMutex(hw_mutex);
}

static void ensure_mutex(void)
{
    if (!hw_mutex) hw_mutex = SDL_CreateMutex();
}

/* ------------------------------------------------------------------------ */
/* Vector table                                                              */
/* ------------------------------------------------------------------------ */

/*
 * On a real PC the BIOS owns every vector before the game takes over, and the
 * game saves what it finds so it can chain to it and restore it on exit. With
 * an empty table those saved pointers would be NULL, and the timer handler
 * calls its saved vector unconditionally -- so the table starts out populated
 * with handlers that do nothing, exactly as if the BIOS were there.
 */
static void bios_stub_handler(void) { }

void interrupts_init(void)
{
    ensure_mutex();

    for (int i = 0; i < VECTOR_COUNT; i++) {
        vectors[i] = bios_stub_handler;
    }
    interrupts_masked = false;
}

interrupt_handler interrupt_get_vector(int number)
{
    if (number < 0 || number >= VECTOR_COUNT) return NULL;
    return vectors[number];
}

void interrupt_set_vector(int number, interrupt_handler handler)
{
    if (number < 0 || number >= VECTOR_COUNT) return;
    vectors[number] = handler;
}

void (*getvect(int interruptno))(void)
{
    return interrupt_get_vector(interruptno);
}

void setvect(int interruptno, void (*isr)(void))
{
    interrupt_set_vector(interruptno, isr);
}

/* ------------------------------------------------------------------------ */
/* Masking                                                                   */
/* ------------------------------------------------------------------------ */

void interrupt_mask(void)
{
    ensure_mutex();
    hardware_lock();
    interrupts_masked = true;
    hardware_unlock();
}

void interrupt_unmask(void)
{
    hardware_lock();
    interrupts_masked = false;
    hardware_unlock();
}

bool interrupts_enabled(void)
{
    return !interrupts_masked;
}

void disable(void) { interrupt_mask(); }
void enable(void)  { interrupt_unmask(); }

/*
 * The game brackets critical sections with `pushf` / `popf`, saving and later
 * restoring the flags register. Only the interrupt flag matters to the code
 * around them, so that is all these carry. See patches/0003.
 */
unsigned long SaveInterruptFlag(void)
{
    unsigned long saved;

    hardware_lock();
    saved = interrupts_masked ? 0UL : 1UL;
    hardware_unlock();

    return saved;
}

void RestoreInterruptFlag(unsigned long saved)
{
    hardware_lock();
    interrupts_masked = (saved == 0UL);
    hardware_unlock();
}

bool interrupt_deliver(int number)
{
    interrupt_handler handler;

    ensure_mutex();
    hardware_lock();

    if (interrupts_masked) {
        hardware_unlock();
        return false;
    }

    handler = interrupt_get_vector(number);
    if (handler) handler();

    hardware_unlock();
    return handler != NULL;
}

/* ------------------------------------------------------------------------ */
/* BIOS services                                                             */
/* ------------------------------------------------------------------------ */

/*
 * int 10h, the video BIOS. The game reaches it two ways: through int86() to
 * query the current mode, and through geninterrupt() after loading the pseudo
 * registers. Only the handful of functions it actually calls are implemented.
 */
static void bios_video(unsigned char ah, unsigned char al,
                       unsigned char bh, unsigned char bl,
                       unsigned char *out_al)
{
    switch (ah) {
    case 0x00:  /* set video mode */
        ega_set_video_mode(al);
        break;

    case 0x05:  /* select active display page */
        ega_select_active_page(al);
        platform_page_flipped();
        break;

    case 0x0F:  /* get video state: AL receives the current mode */
        if (out_al) *out_al = 0x0D;
        break;

    case 0x10:  /* palette */
        if (al == 0x00) {
            ega_set_palette_register(bl, bh);
        } else if (al == 0x01) {
            ega_set_border_color(bh);
        }
        break;

    default:
        break;
    }
}

int int86(int intno, union REGS *inregs, union REGS *outregs)
{
    unsigned char al = inregs->h.al;

    if (intno == 0x10) {
        bios_video(inregs->h.ah, inregs->h.al,
                   inregs->h.bh, inregs->h.bl, &al);
    }

    if (outregs) {
        if (outregs != inregs) *outregs = *inregs;
        outregs->h.al = al;
        outregs->x.cflag = 0;
    }

    return al;
}

/*
 * geninterrupt() fires an interrupt with arguments already staged in the
 * pseudo registers, so the values come from there rather than a REGS struct.
 */
void geninterrupt(int intno)
{
    if (intno == 0x10) {
        unsigned char ah = (unsigned char)(_AH ? _AH : (_AX >> 8));
        unsigned char al = (unsigned char)(_AL ? _AL : (_AX & 0xFF));
        unsigned char out_al = al;

        bios_video(ah, al, _BH, _BL, &out_al);
        _AL = out_al;
        return;
    }

    /* Any other software interrupt goes to whatever the game installed. */
    interrupt_deliver(intno);
}

/* ------------------------------------------------------------------------ */
/* Console                                                                   */
/* ------------------------------------------------------------------------ */

/*
 * The game calls these only on paths that leave graphics mode: the exit
 * screen, and the "not an AT" prompt that a modern machine never reaches.
 */
void textmode(int newmode)
{
    (void)newmode;
}

int getch(void)
{
    int c = getchar();
    return (c == EOF) ? 0 : c;
}
