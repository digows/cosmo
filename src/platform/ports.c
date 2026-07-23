/*
 * ports.c -- I/O port dispatch.
 *
 * Every outportb/inportb/outport in the game lands here and is routed to the
 * emulated device that owns the address. The full set the game touches is
 * small: the EGA, the PIT, the interrupt controller, the keyboard, the PC
 * speaker gate, the joystick and the AdLib.
 */

#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "cosmo/dos_compat.h"
#include "cosmo/ega.h"
#include "cosmo/hardware.h"
#include "cosmo/opl.h"

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

/*
 * A lock-free snapshot of the speaker for the audio thread. The game updates
 * the speaker from the timer interrupt, which runs on the main thread, so this
 * is a genuine cross-thread handoff.
 */
static SDL_AtomicInt speaker_snapshot;

static void speaker_publish(void)
{
    uint32_t packed = pit2.divisor;

    /* Both gate bits of port 0x61 must be set for the speaker to sound. */
    if ((kbd.port61 & 0x03) == 0x03) packed |= 1u << 16;

    SDL_SetAtomicInt(&speaker_snapshot, (int)packed);
}

uint32_t speaker_state(void)
{
    return (uint32_t)SDL_GetAtomicInt(&speaker_snapshot);
}

void PlatformDelayMicroseconds(unsigned microseconds)
{
    uint64_t end = SDL_GetTicksNS() + (uint64_t)microseconds * 1000u;

    /*
     * Busy-wait. These are tens of microseconds and always on the game thread,
     * so handing the scheduler a sleep request that rounds up to a millisecond
     * would be both slower and less accurate than simply spinning.
     */
    while (SDL_GetTicksNS() < end) {
        /* VOID */
    }
}

unsigned long adlib_writes;
static uint8_t adlib_register_index;
static uint8_t adlib_registers[256];

/*
 * The OPL2's two timers, which are what AdLib detection actually probes: the
 * game starts timer 1, waits, and looks for its overflow flag in the status
 * register. They are modelled here rather than in the synthesiser because they
 * are a chip housekeeping function, unrelated to making sound.
 *
 * Timer 1 counts in 80 microsecond steps and timer 2 in 320, each from its
 * preset up to 256.
 */
#define OPL_TIMER1_STEP_NS  80000u
#define OPL_TIMER2_STEP_NS 320000u

static struct {
    uint8_t preset1, preset2;
    uint8_t control;        /* register 0x04 */
    uint64_t start1_ns, start2_ns;
    bool running1, running2;
    bool flag1, flag2;
} opl;

static void opl_timer_write_control(uint8_t value)
{
    /*
     * Bit 7 is IRQ RESET, and the datasheet requires it to be set on its own:
     * when present it clears the flags and nothing else happens.
     */
    if (value & 0x80) {
        opl.flag1 = opl.flag2 = false;
        return;
    }

    opl.control = value;

    if ((value & 0x01) && !opl.running1) {
        opl.start1_ns = SDL_GetTicksNS();
        opl.running1 = true;
    } else if (!(value & 0x01)) {
        opl.running1 = false;
    }

    if ((value & 0x02) && !opl.running2) {
        opl.start2_ns = SDL_GetTicksNS();
        opl.running2 = true;
    } else if (!(value & 0x02)) {
        opl.running2 = false;
    }
}

static uint8_t opl_status(void)
{
    uint64_t now = SDL_GetTicksNS();
    uint8_t status = 0;

    /* A masked timer still runs; it just cannot raise its flag. */
    if (opl.running1 && !(opl.control & 0x40)) {
        uint64_t period = (uint64_t)(256 - opl.preset1) * OPL_TIMER1_STEP_NS;
        if (now - opl.start1_ns >= period) opl.flag1 = true;
    }

    if (opl.running2 && !(opl.control & 0x20)) {
        uint64_t period = (uint64_t)(256 - opl.preset2) * OPL_TIMER2_STEP_NS;
        if (now - opl.start2_ns >= period) opl.flag2 = true;
    }

    if (opl.flag1) status |= 0x40;
    if (opl.flag2) status |= 0x20;
    if (status) status |= 0x80;   /* bit 7 is the wired-or of both flags */

    return status;
}

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
    case 0x0042: pit_data(2, value); speaker_publish(); break;
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
        speaker_publish();
        break;

    /* Joystick: writing starts the resistor timing cycle */
    case 0x0201:
        break;

    /* AdLib (OPL2) */
    case 0x0388: adlib_register_index = value; break;
    case 0x0389:
        adlib_writes++;
        adlib_registers[adlib_register_index] = value;

        switch (adlib_register_index) {
        case 0x02: opl.preset1 = value; break;
        case 0x03: opl.preset2 = value; break;
        case 0x04: opl_timer_write_control(value); break;
        default: break;
        }

        /*
         * The synthesiser sees every write, including the timer registers.
         * They are harmless to it, and passing them through keeps this from
         * having to know which registers make sound.
         */
        opl_write(adlib_register_index, value);
        if (SDL_getenv("COSMO_OPL_LOG")) {
            fprintf(stderr, "opl %02X=%02X\n", adlib_register_index, value);
        }
        break;

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

    /* AdLib status: the two timer overflow flags and their wired-or. */
    case 0x0388:
        return opl_status();

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
    /*
     * Port 0x60 latches the last scancode the controller saw, and the game
     * tests it directly: IsAnyKeyDown() is `!(inportb(0x60) & 0x80)`, so a
     * zero here reads as a key being held down and the title screen falls
     * straight through its idle loop. On a real machine the last thing seen
     * before the game started was a key being released, so start with a
     * release code.
     */
    kbd.scancode = 0x80;
    memset(adlib_registers, 0, sizeof adlib_registers);
    memset(&opl, 0, sizeof opl);
    adlib_register_index = 0;
    opl_init();
    speaker_publish();
}
