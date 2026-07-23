/*
 * font.h -- Drawing text with the game's own font.
 *
 * FONTS.MNI holds 100 characters as masked sprite tiles: 8 rows of five bytes,
 * a transparency mask followed by one byte per EGA plane. The game draws text
 * by blitting these; so does this, so the launcher looks like it belongs to
 * the game rather than to the host.
 */

#ifndef COSMO_FONT_H
#define COSMO_FONT_H

#include <stdbool.h>
#include <stdint.h>

/* Screen dimensions in font tiles. */
#define FONT_COLUMNS 40
#define FONT_ROWS    25

/* Load FONTS.MNI for `episode` out of `datadir`. */
bool font_load(const char *datadir, int episode);

/* Draw `text` with its first character at tile position x, y. */
void font_draw(int x, int y, const char *text);

/* Draw `text` centred across the screen. */
void font_draw_centered(int y, const char *text);

/* Fill a rectangle of tiles with the font's grey background tile. */
void font_fill(int x, int y, int width, int height);

#endif /* COSMO_FONT_H */
