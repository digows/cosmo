/*
 * png.c -- Minimal dependency-free PNG writer. See include/cosmo/png.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cosmo/png.h"

#define DEFLATE_MAX_STORED 65535u

static uint32_t crc32_of(const uint8_t *data, size_t len, uint32_t crc)
{
    static uint32_t table[256];
    static bool built = false;

    if (!built) {
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[n] = c;
        }
        built = true;
    }

    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t adler32_of(const uint8_t *data, size_t len)
{
    uint32_t a = 1, b = 0;

    for (size_t i = 0; i < len; i++) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void write_chunk(FILE *fp, const char *type,
                        const uint8_t *data, size_t len)
{
    uint8_t header[4];
    uint8_t trailer[4];
    uint32_t crc;

    put_be32(header, (uint32_t)len);
    fwrite(header, 1, 4, fp);
    fwrite(type, 1, 4, fp);
    if (len) fwrite(data, 1, len, fp);

    crc = crc32_of((const uint8_t *)type, 4, 0);
    if (len) crc = crc32_of(data, len, crc);
    put_be32(trailer, crc);
    fwrite(trailer, 1, 4, fp);
}

/*
 * Wrap `raw` in a zlib stream built entirely from stored (uncompressed)
 * DEFLATE blocks. Caller owns the returned buffer.
 */
static uint8_t *deflate_stored(const uint8_t *raw, size_t raw_len, size_t *out_len)
{
    size_t blocks = (raw_len + DEFLATE_MAX_STORED - 1) / DEFLATE_MAX_STORED;
    size_t size = 2 + blocks * 5 + raw_len + 4;
    uint8_t *out = malloc(size);
    size_t pos = 0, offset = 0;

    if (!out) return NULL;

    out[pos++] = 0x78;  /* CMF: deflate, 32 KiB window */
    out[pos++] = 0x01;  /* FLG: no preset dictionary, check bits valid */

    while (offset < raw_len) {
        size_t chunk = raw_len - offset;
        if (chunk > DEFLATE_MAX_STORED) chunk = DEFLATE_MAX_STORED;

        out[pos++] = (offset + chunk >= raw_len) ? 0x01 : 0x00;  /* BFINAL, stored */
        out[pos++] = (uint8_t)(chunk & 0xFF);
        out[pos++] = (uint8_t)(chunk >> 8);
        out[pos++] = (uint8_t)(~chunk & 0xFF);
        out[pos++] = (uint8_t)((~chunk >> 8) & 0xFF);

        memcpy(out + pos, raw + offset, chunk);
        pos += chunk;
        offset += chunk;
    }

    put_be32(out + pos, adler32_of(raw, raw_len));
    pos += 4;

    *out_len = pos;
    return out;
}

bool png_write_rgb(const char *path, const uint8_t *rgb, int width, int height)
{
    static const uint8_t signature[8] = {137, 'P', 'N', 'G', '\r', '\n', 26, '\n'};
    size_t stride = (size_t)width * 3;
    size_t raw_len = (size_t)height * (1 + stride);
    uint8_t *raw = malloc(raw_len);
    uint8_t *stream = NULL;
    size_t stream_len = 0;
    uint8_t ihdr[13];
    FILE *fp = NULL;
    bool ok = false;

    if (!raw) goto done;

    /* Each scanline is prefixed with its filter type; 0 means None. */
    for (int y = 0; y < height; y++) {
        uint8_t *dst = raw + (size_t)y * (1 + stride);
        *dst = 0;
        memcpy(dst + 1, rgb + (size_t)y * stride, stride);
    }

    stream = deflate_stored(raw, raw_len, &stream_len);
    if (!stream) goto done;

    fp = fopen(path, "wb");
    if (!fp) goto done;

    fwrite(signature, 1, sizeof signature, fp);

    put_be32(ihdr + 0, (uint32_t)width);
    put_be32(ihdr + 4, (uint32_t)height);
    ihdr[8] = 8;    /* bit depth */
    ihdr[9] = 2;    /* color type: truecolor RGB */
    ihdr[10] = 0;   /* deflate */
    ihdr[11] = 0;   /* adaptive filtering */
    ihdr[12] = 0;   /* no interlace */
    write_chunk(fp, "IHDR", ihdr, sizeof ihdr);
    write_chunk(fp, "IDAT", stream, stream_len);
    write_chunk(fp, "IEND", NULL, 0);

    ok = ferror(fp) == 0;

done:
    if (fp) fclose(fp);
    free(stream);
    free(raw);
    return ok;
}
