/*
 * ega.c -- Software EGA implementation.
 *
 * References: IBM PC Hardware Reference Library - Enhanced Graphics Adapter
 * (1984), and the commentary in Cosmore's own lowlevel.asm.
 */

#include <string.h>

#include "cosmo/ega.h"

ega_state ega;

/*
 * Mode 0Dh default palette. In the palette index bit 0 is blue, bit 1 green,
 * bit 2 red and bit 3 intensity, which lines up with Borland's COLORS enum and
 * with planes 0..3 respectively.
 */
static const uint8_t default_palette[16] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
    0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F
};

void ega_reset_registers(void)
{
    ega.seq_index = 0;
    ega.map_mask = 0x0F;
    ega.gc_index = 0;
    ega.set_reset = 0x00;
    ega.enable_set_reset = 0x00;
    ega.data_rotate = 0x00;
    ega.read_map = 0x00;
    ega.mode = 0x00;
    ega.bit_mask = 0xFF;
    memset(ega.latch, 0, sizeof ega.latch);
}

void ega_init(void)
{
    memset(&ega, 0, sizeof ega);
    ega_reset_registers();
    memcpy(ega.palette, default_palette, sizeof default_palette);
    ega.display_start = 0;
    ega.border_color = 0;
}

/* ------------------------------------------------------------------------- */
/* Registers                                                                 */
/* ------------------------------------------------------------------------- */

static void seq_write(uint8_t index, uint8_t value)
{
    if (index == SEQ_MAP_MASK) {
        ega.map_mask = value & 0x0F;
    }
    /* Reset, Clocking Mode, Character Map and Memory Mode do not affect what
     * Cosmo draws; the game only touches them while setting up the mode. */
}

static void gc_write(uint8_t index, uint8_t value)
{
    switch (index) {
    case GC_SET_RESET:        ega.set_reset        = value & 0x0F; break;
    case GC_ENABLE_SET_RESET: ega.enable_set_reset = value & 0x0F; break;
    case GC_DATA_ROTATE:      ega.data_rotate      = value & 0x1F; break;
    case GC_READ_MAP:         ega.read_map         = value & 0x03; break;
    case GC_MODE:             ega.mode             = value;        break;
    case GC_BIT_MASK:         ega.bit_mask         = value;        break;
    default: break;
    }
}

void ega_out_byte(uint16_t port, uint8_t value)
{
    switch (port) {
    case EGA_PORT_SEQ_INDEX: ega.seq_index = value; break;
    case EGA_PORT_SEQ_DATA:  seq_write(ega.seq_index, value); break;
    case EGA_PORT_GC_INDEX:  ega.gc_index = value; break;
    case EGA_PORT_GC_DATA:   gc_write(ega.gc_index, value); break;
    default: break;
    }
}

/*
 * A 16-bit `out dx, ax` sends AL to the port and AH to port+1. The game leans
 * on this constantly to set an index and its data in one instruction.
 */
void ega_out_word(uint16_t port, uint16_t value)
{
    ega_out_byte(port, (uint8_t)(value & 0xFF));
    ega_out_byte((uint16_t)(port + 1), (uint8_t)(value >> 8));
}

uint8_t ega_in_byte(uint16_t port)
{
    switch (port) {
    case EGA_PORT_SEQ_INDEX: return ega.seq_index;
    case EGA_PORT_GC_INDEX:  return ega.gc_index;
    default: return 0xFF;
    }
}

/* ------------------------------------------------------------------------- */
/* Video memory                                                              */
/* ------------------------------------------------------------------------- */

/*
 * Every read loads the latches, even when the returned value is discarded.
 * The game depends on this: DrawSolidTile copies tiles from video memory to
 * video memory with `*dst = *src` under write mode 1, where the CPU data is
 * ignored and what reaches the screen is the latch content the read loaded.
 */
uint8_t ega_read(uint32_t addr)
{
    addr &= (EGA_PLANE_SIZE - 1);

    for (int p = 0; p < EGA_PLANES; p++) {
        ega.latch[p] = ega.vram[p][addr];
    }

    if (ega.mode & 0x08) {
        /* Read mode 1: color compare. Cosmo never uses it, but it is cheap. */
        uint8_t result = 0;

        for (int bit = 0; bit < 8; bit++) {
            uint8_t match = 1;
            for (int p = 0; p < EGA_PLANES; p++) {
                uint8_t plane_bit = (uint8_t)((ega.latch[p] >> bit) & 1);
                uint8_t wanted = (uint8_t)((ega.set_reset >> p) & 1);
                if (plane_bit != wanted) { match = 0; break; }
            }
            if (match) result |= (uint8_t)(1u << bit);
        }
        return result;
    }

    return ega.latch[ega.read_map];
}

static inline uint8_t rotate_right(uint8_t value, uint8_t count)
{
    count &= 7;
    if (count == 0) return value;
    return (uint8_t)((value >> count) | (value << (8 - count)));
}

static inline uint8_t apply_function(uint8_t data, uint8_t latch, uint8_t fn)
{
    switch (fn) {
    case 1: return (uint8_t)(data & latch);
    case 2: return (uint8_t)(data | latch);
    case 3: return (uint8_t)(data ^ latch);
    default: return data;  /* 0 = replace */
    }
}

void ega_write(uint32_t addr, uint8_t value)
{
    uint8_t write_mode = (uint8_t)(ega.mode & 0x03);
    uint8_t fn = (uint8_t)((ega.data_rotate >> 3) & 0x03);
    uint8_t rot = (uint8_t)(ega.data_rotate & 0x07);

    addr &= (EGA_PLANE_SIZE - 1);

    for (int p = 0; p < EGA_PLANES; p++) {
        uint8_t data;
        uint8_t result;

        if (!(ega.map_mask & (1u << p))) continue;

        switch (write_mode) {
        case 1:
            /* Latched write: the CPU data is irrelevant. */
            ega.vram[p][addr] = ega.latch[p];
            continue;

        case 2:
            /* Low four bits are the color; each plane becomes 0x00 or 0xFF. */
            data = ((value >> p) & 1) ? 0xFF : 0x00;
            break;

        case 3:
            /* Unused by Cosmo, and absent from the original EGA. */
            data = rotate_right(value, rot);
            break;

        default: /* write mode 0 */
            data = rotate_right(value, rot);
            if (ega.enable_set_reset & (1u << p)) {
                data = ((ega.set_reset >> p) & 1) ? 0xFF : 0x00;
            }
            break;
        }

        result = apply_function(data, ega.latch[p], fn);
        result = (uint8_t)((result & ega.bit_mask) |
                           (ega.latch[p] & (uint8_t)~ega.bit_mask));

        ega.vram[p][addr] = result;
    }
}

/* ------------------------------------------------------------------------- */
/* BIOS (int 10h) services the game uses                                     */
/* ------------------------------------------------------------------------- */

void ega_set_video_mode(uint8_t mode)
{
    /* The only mode that matters here is 0Dh: 320x200, 16 colors, 8 pages. */
    ega_reset_registers();
    memcpy(ega.palette, default_palette, sizeof default_palette);
    ega.display_start = 0;

    if (mode == 0x0D) {
        memset(ega.vram, 0, sizeof ega.vram);
    }
}

void ega_set_palette_register(uint8_t index, uint8_t color_value)
{
    if (index < 16) ega.palette[index] = (uint8_t)(color_value & 0x3F);
}

void ega_set_border_color(uint8_t color_value)
{
    ega.border_color = (uint8_t)(color_value & 0x3F);
}

void ega_select_active_page(uint8_t page)
{
    ega.display_start = (uint16_t)(page * EGA_PAGE_SIZE);
}
