/*
 * hardware.h -- The emulated PC peripherals behind the I/O ports.
 *
 * The game reaches all of these through inportb/outportb/outport. This header
 * is the interface the rest of the platform layer uses to drive them: the main
 * thread feeds keystrokes in, reads the programmed timer rate back out, and
 * delivers interrupts.
 */

#ifndef COSMO_HARDWARE_H
#define COSMO_HARDWARE_H

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------------ */
/* Programmable Interval Timer (8253)                                       */
/* ------------------------------------------------------------------------ */

/* The base frequency of PIT channel 0, as the game computes it (game2.c:182). */
#define PIT_BASE_HZ 1192030.0

/*
 * Divisor the game last programmed into channel 0. A value of 0 means the full
 * 65536 range, which is the ~18.2 Hz the BIOS default runs at.
 */
uint16_t pit_divisor(void);

/* Interrupts per second implied by the current divisor. */
double pit_frequency(void);

/* ------------------------------------------------------------------------ */
/* Keyboard (8255 PPI + 8042 controller)                                    */
/* ------------------------------------------------------------------------ */

/*
 * Queue an XT scancode for delivery. The game's int 9 handler reads it from
 * port 0x60. Release codes are the make code with bit 7 set.
 */
void keyboard_push_scancode(uint8_t scancode);

/* True while a scancode is waiting to be consumed by the interrupt handler. */
bool keyboard_has_scancode(void);

/* ------------------------------------------------------------------------ */
/* PC speaker                                                               */
/* ------------------------------------------------------------------------ */

/* Current square wave frequency in Hz, or 0 when the speaker is gated off. */
double speaker_frequency(void);

/* ------------------------------------------------------------------------ */
/* Interrupt delivery                                                       */
/* ------------------------------------------------------------------------ */

typedef void (*interrupt_handler)(void);

void interrupts_init(void);
interrupt_handler interrupt_get_vector(int number);
void interrupt_set_vector(int number, interrupt_handler handler);

/*
 * Deliver an interrupt to the game, respecting disable()/enable(). Returns
 * false when interrupts are masked and the request was dropped, exactly as the
 * hardware would have been ignored.
 */
bool interrupt_deliver(int number);

/* disable() / enable() from the game map onto these. */
void interrupt_mask(void);
void interrupt_unmask(void);
bool interrupts_enabled(void);

/* Guards every piece of emulated hardware state against the game thread. */
void hardware_lock(void);
void hardware_unlock(void);
void hardware_init(void);

#endif /* COSMO_HARDWARE_H */
