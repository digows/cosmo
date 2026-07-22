/*
 * png.h -- Minimal dependency-free PNG writer.
 *
 * Screenshots are the main verification tool for this port, so the writer has
 * to work identically everywhere with no optional dependency to chase down on
 * Windows. It emits uncompressed DEFLATE blocks, which are perfectly valid
 * zlib streams, so no compression library is needed at all. Files are larger
 * than they would be with real compression; for 320x200 screenshots that is
 * an entirely acceptable trade.
 */

#ifndef COSMO_PNG_H
#define COSMO_PNG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Write `width` x `height` 24-bit RGB pixels as a PNG file. */
bool png_write_rgb(const char *path, const uint8_t *rgb, int width, int height);

#endif /* COSMO_PNG_H */
