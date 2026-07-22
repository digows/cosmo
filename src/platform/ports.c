/*
 * ports.c -- I/O port dispatch.
 *
 * Every outportb/inportb/outport in the game lands here and is routed to the
 * emulated device that owns the address. The full set the game touches is
 * small: the EGA, the PIT, the interrupt controller, the keyboard, the PC
 * speaker gate, the joystick and the AdLib.
 */

#include <string.h>

#include "cosmo/dos_compat.h"
#include "cosmo/ega.h"
#include "cosmo/hardware.h"

/* ------------------------------------------------------------------------ */
/* Device state                                                             */
/* ------------------------------------------------------------------------ */

static struct {
    uint16_t divisor;
    uint8_t latch_low;      /* first byte of a two-byte divisor write */
    bool expecting_high;
} pit0;

static struct {
    uint16_t divisor;
    uint8_t latch_low;
    bool expecting_high;
} pit2;                      /* channel 2 drives the PC speaker */

static struct {
    uint8_t port61;          /* bit 0 gate, bit 1 output enable, bit 7 ack */
    uint8_t scancode;
    bool pending;
} kbd;

static uint8_t adlib_register_index;
static uint8_t adlib_registers[256];
static uint8_t adlib_status = 0x00;

uint16_t pit_divisor(void)
{
    return pit0.divisor;
}

double pit_frequency(void)
{
    /* A zero divisor means the full 65536-count range. */
    uint32_t divisor = pit0.divisor ? pit0.divisor : 65536u;
    return PIT_BASE_HZ / (double)divisor;
}

void keyboard_push_scancode(uint8_t scancode)
{
    kbd.scancode = scancode;
    kbd.pending = true;
}

bool keyboard_has_scancode(void)
{
    return kbd.pending;
}

double speaker_frequency(void)
{
    /* Bits 0 and 1 of port 0x61 must both be set for the speaker to sound. */
    if ((kbd.port61 & 0x03) != 0x03) return 0.0;
    if (pit2.divisor == 0) return 0.0;

    return PIT_BASE_HZ / (double)pit2.divisor;
}

/* ------------------------------------------------------------------------ */
/* PIT                                                                       */
/* ------------------------------------------------------------------------ */

static void pit_command(uint8_t value)
{
    uint8_t channel = (uint8_t)(value >> 6);

    /*
     * The game only ever uses access mode 3 ("low byte, then high byte"), on
     * channel 0 for the game clock and channel 2 for speaker tones.
     */
    if (channel == 0) {
        pit0.expecting_high = false;
    } else if (channel == 2) {
        pit2.expecting_high = false;
    }
}

static void pit_data(int channel, uint8_t value)
{
    if (channel == 0) {
        if (!pit0.expecting_high) {
            pit0.latch_low = value;
            pit0.expecting_high = true;
        } else {
            pit0.divisor = (uint16_t)((value << 8) | pit0.latch_low);
            pit0.expecting_high = false;
        }
    } else {
        if (!pit2.expecting_high) {
            pit2.latch_low = value;
            pit2.expecting_high = true;
        } else {
            pit2.divisor = (uint16_t)((value << 8) | pit2.latch_low);
            pit2.expecting_high = false;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Port entry points                                                         */
/* ------------------------------------------------------------------------ */

void outportb(int portid, unsigned char value)
{
    switch (portid) {
    /* EGA */
    case EGA_PORT_SEQ_INDEX:
    case EGA_PORT_SEQ_DATA:
    case EGA_PORT_GC_INDEX:
    case EGA_PORT_GC_DATA:
        ega_out_byte((uint16_t)portid, value);
        break;

    /* 8259 interrupt controller: end-of-interrupt, nothing to model */
    case 0x0020:
        break;

    /* PIT */
    case 0x0040: pit_data(0, value); break;
    case 0x0042: pit_data(2, value); break;
    case 0x0043: pit_command(value); break;

    /*
     * Port 0x61. Bit 7 pulses to acknowledge a keyboard byte; bits 0 and 1
     * gate the speaker. The game strobes the acknowledge bit high then low.
     */
    case 0x0061:
        if ((value & 0x80) && !(kbd.port61 & 0x80)) {
            kbd.pending = false;
        }
        kbd.port61 = value;
        break;

    /* Joystick: writing starts the resistor timing cycle */
    case 0x0201:
        break;

    /* AdLib (OPL2) */
    case 0x0388: adlib_register_index = value; break;
    case 0x0389: adlib_registers[adlib_register_index] = value; break;

    default:
        break;
    }
}

unsigned char inportb(int portid)
{
    switch (portid) {
    case EGA_PORT_SEQ_INDEX:
    case EGA_PORT_GC_INDEX:
        return ega_in_byte((uint16_t)portid);

    /* Keyboard data */
    case 0x0060:
        return kbd.scancode;

    case 0x0061:
        return kbd.port61;

    /*
     * Joystick. Bits 4-7 are the button lines, active low, so 0xF0 reports
     * every button released. Bits 0-3 are the axis timers, which read back as
     * zero to mean "no stick attached".
     */
    case 0x0201:
        return 0xF0;

    /*
     * AdLib status. Bits 5-7 are the timer flags the detection routine in
     * game2.c looks for. Reporting zero means no AdLib is present, which is
     * honest until the OPL2 is actually emulated.
     */
    case 0x0388:
        return adlib_status;

    default:
        return 0xFF;
    }
}

/*
 * A 16-bit `out dx, ax` writes AL to the port and AH to the next one up. The
 * game uses it to set an index and its data in a single instruction.
 */
void outport(int portid, int value)
{
    outportb(portid, (unsigned char)(value & 0xFF));
    outportb(portid + 1, (unsigned char)((value >> 8) & 0xFF));
}

void hardware_init(void)
{
    memset(&pit0, 0, sizeof pit0);
    memset(&pit2, 0, sizeof pit2);
    memset(&kbd, 0, sizeof kbd);
    memset(adlib_registers, 0, sizeof adlib_registers);
    adlib_register_index = 0;
    adlib_status = 0x00;
}
