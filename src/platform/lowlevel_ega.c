/*
 * lowlevel_ega.c -- The game's low-level drawing routines, on emulated hardware.
 *
 * The original lowlevel.asm talks straight to the EGA: it pokes the Sequencer
 * and Graphics Controller and stores into segment 0xA000. Upstream publishes a
 * pure C translation of every one of those procedures in C-DRAWING.md; this
 * file is that translation with the far pointer arithmetic replaced by calls
 * into the emulated adapter.
 *
 * The logic is otherwise unchanged, deliberately -- including the reads whose
 * value is discarded but whose side effect (loading the EGA latches) is the
 * entire point.
 */

#include "glue.h"

#include "cosmo/ega.h"
#include "cosmo/video.h"

#include <string.h>
/* Tile image banks live above the display pages, at 0xA400 in segment terms. */
#define TILE_BANK_BASE  ((0xA400u - 0xA000u) << 4)

static word draw_page_number;
static dword draw_page_base;

word WideTerrainColumns(void)
{
    return video_is_widescreen() ? EGA_WIDE_COLS : 0;
}

void BeginWideTerrain(void)
{
    if (draw_page_number >= 2) return;
    ega_wide_terrain *page = &ega_wide_pages[draw_page_number];
    memset(page->planes, 0, sizeof page->planes);
    page->visible = 1;
}

void ClearWideTerrain(void)
{
    ega_wide_pages[0].visible = 0;
    ega_wide_pages[1].visible = 0;
}

void DrawWideTerrainTile(word solid, const byte *masked, word x, word y)
{
    if (draw_page_number >= 2 || x >= EGA_WIDE_COLS || y >= EGA_WIDE_ROWS) return;
    ega_wide_terrain *page = &ega_wide_pages[draw_page_number];
    for (unsigned row = 0; row < 8; ++row) {
        size_t dst = (y * 8u + row) * EGA_WIDE_COLS + x;
        unsigned src = (TILE_BANK_BASE + solid + row) & (EGA_PLANE_SIZE - 1);
        for (unsigned plane = 0; plane < EGA_PLANES; ++plane) {
            uint8_t bits = ega.vram[plane][src];
            if (masked) bits = (uint8_t)((bits & masked[row * 5]) | masked[row * 5 + 1 + plane]);
            page->planes[plane][dst] = bits;
        }
    }
}

static bool wide_tile_target(word x, word y, size_t *offset)
{
    if (draw_page_number >= 2 || !ega_wide_pages[draw_page_number].visible ||
        x < EGA_WIDE_FIRST_COLUMN || x >= EGA_WIDE_FIRST_COLUMN + EGA_WIDE_COLS ||
        y == 0 || y > EGA_WIDE_ROWS) return false;
    *offset = (y - 1u) * 8u * EGA_WIDE_COLS + x - EGA_WIDE_FIRST_COLUMN;
    return true;
}

static bool draw_wide_sprite(const byte *src, word x, word y, word mode)
{
    size_t offset;
    if (!wide_tile_target(x, y, &offset))
        return x >= EGA_SCREEN_W / 8 && video_is_widescreen();
    ega_wide_terrain *page = &ega_wide_pages[draw_page_number];
    for (unsigned row = 0; row < 8; ++row) {
        unsigned source_row = mode == DRAW_MODE_FLIPPED ? 7 - row : row;
        const byte *pixels = src + source_row * 5;
        size_t dst = offset + row * EGA_WIDE_COLS;
        if (mode == DRAW_MODE_TRANSLUCENT) {
            page->planes[3][dst] |= (uint8_t)~pixels[0];
        } else {
            for (unsigned plane = 0; plane < EGA_PLANES; ++plane) {
                uint8_t foreground = mode == DRAW_MODE_WHITE
                                     ? (uint8_t)~pixels[0] : pixels[plane + 1];
                page->planes[plane][dst] =
                    (uint8_t)((page->planes[plane][dst] & pixels[0]) | foreground);
            }
        }
    }
    return true;
}

static bool lighten_wide_tile(word x, word y, unsigned mask, bool grow, bool shrink)
{
    size_t offset;
    if (!wide_tile_target(x, y, &offset))
        return x >= EGA_SCREEN_W / 8 && video_is_widescreen();
    uint8_t *intensity = ega_wide_pages[draw_page_number].planes[3];
    for (unsigned row = 0; row < 8; ++row) {
        intensity[offset + row * EGA_WIDE_COLS] |= (uint8_t)mask;
        if (grow) mask = (mask << 1) | 1;
        if (shrink) mask = (mask >> 1) | 0x80;
    }
    return true;
}

void SetVideoMode(word mode_num)
{
    ega_set_video_mode((uint8_t)mode_num);

    ega_out_word(EGA_PORT_GC_INDEX, (0x00 << 8) | 0x07);
    ega_out_byte(EGA_PORT_SEQ_INDEX, 0x02);
}

void SetBorderColorRegister(word color_value)
{
    ega_set_border_color((uint8_t)color_value);
}

void SetPaletteRegister(word palette_index, word color_value)
{
    ega_set_palette_register((uint8_t)palette_index, (uint8_t)color_value);
}

void UpdateDrawPageSegment(void)
{
    draw_page_base = (dword)draw_page_number * EGA_PAGE_SIZE;
}

void SelectDrawPage(word page_num)
{
    draw_page_number = page_num;
    UpdateDrawPageSegment();
}

void SelectActivePage(word page_num)
{
    ega_select_active_page((uint8_t)page_num);
    platform_page_flipped();
}

/*
 * Copy an 8x8 solid tile from the tile bank to the draw page. Under write
 * mode 1 the CPU data is ignored: the read loads the latches and the write
 * dumps them straight back out, moving all four planes at once.
 */
void DrawSolidTile(word src_offset, word dst_offset)
{
    dword src = TILE_BANK_BASE + src_offset;
    dword dst = draw_page_base + dst_offset;

    for (int row = 0; row < 8; row++) {
        ega_write(dst, ega_read(src));
        src++;
        dst += 40;
    }
}

void DrawSpriteTileTranslucent(byte *src, word x, word y)
{
    if (draw_wide_sprite(src, x, y, DRAW_MODE_TRANSLUCENT)) return;
    dword dst = draw_page_base + x + (dword)y * 320;

    ega_out_byte(EGA_PORT_SEQ_INDEX, 0x02);

    for (int row = 0; row < 8; row++) {
        ega_out_word(EGA_PORT_GC_INDEX, (uint16_t)((~(*src) << 8) | 0x08));
        ega_out_byte(EGA_PORT_SEQ_DATA, 0x08);

        (void)ega_read(dst);   /* loads the latches */
        ega_write(dst, 0xFF);

        src += 5;
        dst += 40;
    }
}

void LightenScreenTileWest(word x, word y)
{
    if (lighten_wide_tile(x, y, 0x01, true, false)) return;
    dword dst = draw_page_base + x + yOffsetTable[y];
    byte mask = 0x01;

    ega_out_byte(EGA_PORT_SEQ_DATA, 0x08);

    for (int row = 0; row < 8; row++) {
        ega_out_word(EGA_PORT_GC_INDEX, (uint16_t)((mask << 8) | 0x08));

        (void)ega_read(dst);
        ega_write(dst, mask);

        dst += 40;
        mask = (uint8_t)((mask << 1) | 0x01);
    }
}

void LightenScreenTile(word x, word y)
{
    if (lighten_wide_tile(x, y, 0xff, false, false)) return;
    dword dst = draw_page_base + x + yOffsetTable[y];

    ega_out_word(EGA_PORT_GC_INDEX, (0xFF << 8) | 0x08);  /* EGA_BIT_MASK_DEFAULT */
    ega_out_byte(EGA_PORT_SEQ_DATA, 0x08);

    for (int row = 0; row < 8; row++) {
        ega_write(dst, 0xFF);
        dst += 40;
    }
}

void LightenScreenTileEast(word x, word y)
{
    if (lighten_wide_tile(x, y, 0x80, false, true)) return;
    dword dst = draw_page_base + x + yOffsetTable[y];
    byte mask = 0x80;

    ega_out_byte(EGA_PORT_SEQ_INDEX, 0x02);

    for (int row = 0; row < 8; row++) {
        ega_out_word(EGA_PORT_GC_INDEX, (uint16_t)((mask << 8) | 0x08));

        (void)ega_read(dst);
        ega_write(dst, mask);

        dst += 40;
        mask = (uint8_t)((mask >> 1) | 0x80);
    }
}

/*
 * Masked sprite blit. Each source row holds five bytes: a transparency mask
 * followed by one byte per plane. The read-modify-write punches a hole with
 * the mask and drops the color in.
 */
void DrawSpriteTile(byte *src, word x, word y)
{
    if (draw_wide_sprite(src, x, y, DRAW_MODE_NORMAL)) return;
    dword dst = draw_page_base + x + yOffsetTable[y];

    for (int plane = 0; plane < 4; plane++) {
        byte *localsrc = src;
        dword localdst = dst;
        byte planemask = (uint8_t)(1u << plane);

        ega_out_word(EGA_PORT_SEQ_INDEX, (uint16_t)((planemask << 8) | 0x02));
        ega_out_word(EGA_PORT_GC_INDEX, (uint16_t)((plane << 8) | 0x04));

        for (int row = 0; row < 8; row++) {
            byte existing = ega_read(localdst);
            ega_write(localdst,
                      (uint8_t)((existing & *localsrc) | *(localsrc + plane + 1)));

            localsrc += 5;
            localdst += 40;
        }
    }
}

void DrawMaskedTile(byte *src, word x, word y)
{
    dword dst = draw_page_base + x + yOffsetTable[y];

    ega_out_word(EGA_PORT_GC_INDEX, (0x00 << 8) | 0x05);  /* EGA_MODE_DEFAULT */

    for (int plane = 0; plane < 4; plane++) {
        byte *localsrc = src - 16000;
        dword localdst = dst;
        byte planemask = (uint8_t)(1u << plane);

        ega_out_word(EGA_PORT_SEQ_INDEX, (uint16_t)((planemask << 8) | 0x02));
        ega_out_word(EGA_PORT_GC_INDEX, (uint16_t)((plane << 8) | 0x04));

        for (int row = 0; row < 8; row++) {
            byte existing = ega_read(localdst);
            ega_write(localdst,
                      (uint8_t)((existing & *localsrc) | *(localsrc + plane + 1)));

            localsrc += 5;
            localdst += 40;
        }
    }

    /* EGA_MODE_LATCHED_WRITE() */
    ega_out_word(EGA_PORT_SEQ_INDEX, (0x0F << 8) | 0x02);
    ega_out_word(EGA_PORT_GC_INDEX, (0x01 << 8) | 0x05);
}

void DrawSpriteTileFlipped(byte *src, word x, word y)
{
    if (draw_wide_sprite(src, x, y, DRAW_MODE_FLIPPED)) return;
    dword dst = draw_page_base + x + yOffsetTable[y];

    for (int plane = 0; plane < 4; plane++) {
        byte *localsrc = src;
        dword localdst = dst + 280;
        byte planemask = (uint8_t)(1u << plane);

        ega_out_word(EGA_PORT_SEQ_INDEX, (uint16_t)((planemask << 8) | 0x02));
        ega_out_word(EGA_PORT_GC_INDEX, (uint16_t)((plane << 8) | 0x04));

        for (int row = 0; row < 8; row++) {
            byte existing = ega_read(localdst);
            ega_write(localdst,
                      (uint8_t)((existing & *localsrc) | *(localsrc + plane + 1)));

            localsrc += 5;
            localdst -= 40;
        }
    }
}

void DrawSpriteTileWhite(byte *src, word x, word y)
{
    if (draw_wide_sprite(src, x, y, DRAW_MODE_WHITE)) return;
    dword dst = draw_page_base + x + yOffsetTable[y];

    ega_out_byte(EGA_PORT_SEQ_DATA, 0x0F);

    ega_out_word(EGA_PORT_GC_INDEX, (0x10 << 8) | 0x03);
    ega_out_word(EGA_PORT_GC_INDEX, (0x08 << 8) | 0x05);

    for (int row = 0; row < 8; row++) {
        byte existing = ega_read(dst);
        ega_write(dst, (uint8_t)(existing & ~(*src)));

        src += 5;
        dst += 40;
    }

    ega_out_word(EGA_PORT_GC_INDEX, (0x00 << 8) | 0x03);
    ega_out_word(EGA_PORT_GC_INDEX, (0x00 << 8) | 0x05);  /* EGA_MODE_DEFAULT */
}

/*
 * The original probes the CPU with a series of instruction-timing tricks. The
 * game only branches on this to refuse to start on an 8086, so reporting a 386
 * is both accurate in spirit and what upstream's C translation does.
 */
word GetProcessorType(void)
{
    return 7;  /* CPU_TYPE_80386 */
}

/* Direct video memory access for the two patched call sites in game1.c. */
void dos_vram_write(unsigned offset, unsigned char value)
{
    ega_write(offset, value);
}

unsigned char dos_vram_read(unsigned offset)
{
    return ega_read(offset);
}
