/*
 * ega.h -- Software emulation of the IBM Enhanced Graphics Adapter.
 *
 * Cosmo draws by talking straight to the hardware: it programs the Sequencer
 * and Graphics Controller through I/O ports and writes into video memory at
 * segment 0xA000. Here that memory becomes four 64 KiB planes in ordinary RAM,
 * and every access goes through ega_read()/ega_write(), which reproduce the
 * write modes, latches and bit mask of the real chip.
 *
 * Addresses are linear within video memory (0 == start of 0xA000), not seg:ofs.
 */

#ifndef COSMO_EGA_H
#define COSMO_EGA_H

#include <stdint.h>

#define EGA_PLANES        4
#define EGA_PLANE_SIZE    0x10000u

/* Memory layout used by the game (see graphics.h in the Cosmore sources). */
#define EGA_PAGE_SIZE     0x2000u   /* distance between display pages */
#define EGA_PAGE_BYTES    8000u     /* 320x200 / 8 pixels per byte, per plane */

#define EGA_SCREEN_W      320
#define EGA_SCREEN_H      200

/* Ports */
#define EGA_PORT_SEQ_INDEX  0x03C4
#define EGA_PORT_SEQ_DATA   0x03C5
#define EGA_PORT_GC_INDEX   0x03CE
#define EGA_PORT_GC_DATA    0x03CF

/* Graphics Controller register indices */
#define GC_SET_RESET        0x00
#define GC_ENABLE_SET_RESET 0x01
#define GC_COLOR_COMPARE    0x02
#define GC_DATA_ROTATE      0x03
#define GC_READ_MAP         0x04
#define GC_MODE             0x05
#define GC_MISC             0x06
#define GC_COLOR_DONT_CARE  0x07
#define GC_BIT_MASK         0x08

/* Sequencer register indices */
#define SEQ_MAP_MASK        0x02

typedef struct {
    uint8_t vram[EGA_PLANES][EGA_PLANE_SIZE];
    uint8_t latch[EGA_PLANES];

    /* Sequencer */
    uint8_t seq_index;
    uint8_t map_mask;         /* bits 0-3: planes enabled for writing */

    /* Graphics Controller */
    uint8_t gc_index;
    uint8_t set_reset;
    uint8_t enable_set_reset;
    uint8_t data_rotate;      /* bits 0-2 rotate count, bits 3-4 logic function */
    uint8_t read_map;
    uint8_t mode;             /* bits 0-1 write mode, bit 3 read mode */
    uint8_t bit_mask;

    /* Attribute Controller / CRTC -- only what the game touches */
    uint8_t palette[16];      /* palette index -> 6-bit EGA color value */
    uint16_t display_start;   /* linear offset of the displayed page */
    uint8_t border_color;
} ega_state;

extern ega_state ega;

void    ega_init(void);
void    ega_reset_registers(void);

/*
 * Indexed register writes. The game uses `outport(port, (data << 8) | index)`,
 * which on the 8086 sends the low byte to the index port and the high byte to
 * the data port in a single instruction.
 */
void    ega_out_word(uint16_t port, uint16_t value);
void    ega_out_byte(uint16_t port, uint8_t value);
uint8_t ega_in_byte(uint16_t port);

/* Video memory access. `addr` is linear from 0xA000:0000. */
uint8_t ega_read(uint32_t addr);
void    ega_write(uint32_t addr, uint8_t value);

/* Convert a seg:ofs pair from the original code into a linear address. */
static inline uint32_t ega_addr(uint16_t seg, uint16_t ofs)
{
    return ((uint32_t)(seg - 0xA000u) << 4) + ofs;
}

/* BIOS int 10h services the game relies on. */
void    ega_set_video_mode(uint8_t mode);
void    ega_set_palette_register(uint8_t index, uint8_t color_value);
void    ega_set_border_color(uint8_t color_value);
void    ega_select_active_page(uint8_t page);

/* Convert a 6-bit EGA color value to 8-bit RGB as the display would show it. */
void    ega_palette_rgb(uint8_t value, uint8_t rgb[3]);

#endif /* COSMO_EGA_H */
