/*
 * font.c -- Drawing text with the game's own font.
 *
 * The blitting here is DrawSpriteTile() from the game, reduced to what text
 * needs: read the destination, punch a hole with the mask, drop the colour in,
 * one EGA plane at a time.
 */

#include <string.h>

#include "cosmo/ega.h"
#include "cosmo/font.h"
#include "cosmo/group.h"

#define FONT_BYTES        4000
#define FONT_TILE_BYTES     40

/*
 * Offsets into the font, from the game's graphics.h. Characters below 'a' are
 * indexed from the CP437 up arrow at 0x18; lowercase has its own run.
 */
#define FONT_UP_ARROW        0x0050
#define FONT_LOWER_A         0x0ac8
#define FONT_BACKGROUND_GRAY 0x0f28

static uint8_t font_data[FONT_BYTES];
static bool font_ready;

bool font_load(const char *datadir, int episode)
{
    size_t got = group_read(datadir, episode, "FONTS.MNI",
                            font_data, sizeof font_data);

    if (got != FONT_BYTES) return false;

    /*
     * The game inverts every fifth byte on load, turning the stored mask into
     * the form the blit wants. See LoadFontTileData().
     */
    for (size_t i = 0; i < FONT_BYTES; i += 5) {
        font_data[i] = (uint8_t)~font_data[i];
    }

    font_ready = true;
    return true;
}

static void draw_tile(const uint8_t *src, int x, int y)
{
    uint32_t dst = (uint32_t)x + (uint32_t)y * 320;

    if (x < 0 || x >= FONT_COLUMNS || y < 0 || y >= FONT_ROWS) return;

    for (int plane = 0; plane < 4; plane++) {
        const uint8_t *s = src;
        uint32_t d = dst;

        ega_out_word(EGA_PORT_SEQ_INDEX,
                     (uint16_t)(((1u << plane) << 8) | SEQ_MAP_MASK));
        ega_out_word(EGA_PORT_GC_INDEX, (uint16_t)((plane << 8) | GC_READ_MAP));

        for (int row = 0; row < 8; row++) {
            uint8_t existing = ega_read(d);

            ega_write(d, (uint8_t)((existing & *s) | *(s + plane + 1)));

            s += 5;
            d += 40;
        }
    }
}

static const uint8_t *tile_for(char c)
{
    unsigned offset;

    if (c >= 'a' && c <= 'z') {
        offset = FONT_LOWER_A + (unsigned)(c - 'a') * FONT_TILE_BYTES;
    } else {
        offset = FONT_UP_ARROW +
                 (unsigned)((unsigned char)c - 0x18) * FONT_TILE_BYTES;
    }

    if (offset + FONT_TILE_BYTES > FONT_BYTES) return NULL;
    return font_data + offset;
}

void font_draw(int x, int y, const char *text)
{
    if (!font_ready) return;

    /* Write mode 0, which is what the read-modify-write blit above assumes. */
    ega_out_word(EGA_PORT_GC_INDEX, (0x00 << 8) | GC_MODE);

    for (int i = 0; text[i] != '\0'; i++) {
        const uint8_t *tile = tile_for(text[i]);

        if (tile) draw_tile(tile, x + i, y);
    }
}

void font_draw_centered(int y, const char *text)
{
    int length = (int)strlen(text);
    int x = (FONT_COLUMNS - length) / 2;

    font_draw(x < 0 ? 0 : x, y, text);
}

void font_fill(int x, int y, int width, int height)
{
    if (!font_ready) return;

    ega_out_word(EGA_PORT_GC_INDEX, (0x00 << 8) | GC_MODE);

    for (int row = 0; row < height; row++) {
        for (int col = 0; col < width; col++) {
            draw_tile(font_data + FONT_BACKGROUND_GRAY, x + col, y + row);
        }
    }
}
