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
static bool preview_player_drawn;
static bool preview_star_drawn;

void BeginPreviewPlayerFrame(void)
{
    video_preview_begin_player(draw_page_number);
    preview_player_drawn = false;
    preview_star_drawn = false;
}

int BeginPreviewPlayer(int x, int y)
{
    preview_player_drawn = video_preview_player_begin(draw_page_number, x, y, false);
    return preview_player_drawn;
}

int BeginPreviewAngel(int x, int y)
{
    preview_player_drawn = video_preview_player_begin(draw_page_number, x, y, true);
    return preview_player_drawn;
}

void TrackPreviewPlayer(int x, int y)
{
    video_preview_player_track(draw_page_number, x, y);
}

void DrawPreviewPlayerTile(const byte *pixels, word x, word y, int white)
{
    video_preview_player_tile(draw_page_number, pixels, x, y, white != 0);
}

void OccludePreviewPlayerTile(word x, word y)
{
    for (unsigned row = 0; row < 8; ++row) {
        video_preview_player_occlude(draw_page_number, x, y * 8u + row, 0xff);
        video_preview_star_occlude(draw_page_number, x, y * 8u + row, 0xff);
    }
}

int BeginPreviewStar(dword identity, int x, int y, int spin)
{
    bool captured = video_preview_star_begin(draw_page_number, identity, x, y,
                                             spin != 0, false, false, false);
    preview_star_drawn |= captured;
    return captured;
}

void DrawPreviewStarTile(const byte *pixels, word x, word y)
{
    video_preview_star_tile(draw_page_number, pixels, x, y, false);
}

int BeginPreviewEffect(dword identity, int x, int y, word mode, int expand, int edge_padding)
{
    bool captured = video_preview_star_begin(draw_page_number, identity, x, y,
                                            false, mode == DRAW_MODE_IN_FRONT, expand != 0,
                                            edge_padding != 0);
    preview_star_drawn |= captured;
    return captured;
}

void MarkPreviewEffectSlowRise(void)
{
    video_preview_effect_mark_slow_rise(draw_page_number);
}

void DrawPreviewEffectTile(const byte *pixels, word x, word y, word mode)
{
    byte flipped[40];
    if (mode == DRAW_MODE_FLIPPED) {
        for (unsigned row = 0; row < 8; ++row)
            memcpy(flipped + row * 5, pixels + (7 - row) * 5, 5);
        pixels = flipped;
    }
    video_preview_star_tile(draw_page_number, pixels, x, y, mode == DRAW_MODE_WHITE);
}

static bool is_backdrop_tile(unsigned address)
{
    return address >= EGA_OFFSET_BDROP_EVEN &&
           address < EGA_OFFSET_BDROP_ODD_XY + BACKDROP_SIZE_EGA_MEM;
}

static void mark_preview_tile(unsigned x, unsigned y, const byte *pixels,
                              bool flipped)
{
    for (unsigned row = 0; row < 8; ++row) {
        uint8_t *mask = video_preview_mask_row(draw_page_number, y + row);
        if (!mask || x >= EGA_WIDE_FIRST_COLUMN + EGA_WIDE_COLS) return;
        uint8_t opaque = pixels ? (uint8_t)~pixels[(flipped ? 7 - row : row) * 5] : 0xff;
        mask[x] |= opaque;
        if (preview_player_drawn)
            video_preview_player_occlude(draw_page_number, x, y + row, opaque);
        if (preview_star_drawn)
            video_preview_star_occlude(draw_page_number, x, y + row, opaque);
    }
}

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
        uint8_t *mask = video_preview_mask_row(draw_page_number, (y + 1) * 8u + row);
        if (mask) {
            mask[EGA_WIDE_FIRST_COLUMN + x] = is_backdrop_tile(src)
                ? (masked ? (uint8_t)~masked[row * 5] : 0) : 0xff;
        }
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
    mark_preview_tile(x, y * 8u, src, mode == DRAW_MODE_FLIPPED);
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
    mark_preview_tile(x, y * 8u, NULL, false);
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
        uint8_t *mask = video_preview_mask_row(draw_page_number, dst_offset / 40u + row);
        if (mask) mask[dst_offset % 40u] = is_backdrop_tile(src) ? 0 : 0xff;
        ega_write(dst, ega_read(src));
        src++;
        dst += 40;
    }
}

void DrawSpriteTileTranslucent(byte *src, word x, word y)
{
    if (draw_wide_sprite(src, x, y, DRAW_MODE_TRANSLUCENT)) return;
    mark_preview_tile(x, y * 8u, src, false);
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
    mark_preview_tile(x, y * 8u, NULL, false);
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
    mark_preview_tile(x, y * 8u, NULL, false);
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
    mark_preview_tile(x, y * 8u, NULL, false);
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
    mark_preview_tile(x, y * 8u, src, false);
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
    mark_preview_tile(x, y * 8u, src - 16000, false);
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
    mark_preview_tile(x, y * 8u, src, true);
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
    mark_preview_tile(x, y * 8u, src, false);
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
