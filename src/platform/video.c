/*
 * video.c -- Converte a VRAM planar do EGA emulado em pixels e apresenta.
 *
 * O EGA guarda cada pixel espalhado em 4 planos: o bit N de cada plano forma o
 * bit N do indice de paleta. Aqui isso e desentrelacado, passado pela paleta de
 * 6 bits do EGA e entregue como RGB.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include <SDL2/SDL.h>

#include "ega.h"
#include "video.h"

#define FB_PIXELS (EGA_SCREEN_W * EGA_SCREEN_H)

static SDL_Window   *window;
static SDL_Renderer *renderer;
static SDL_Texture  *texture;

/*
 * Um valor de cor EGA tem 6 bits: r'g'b'RGB, onde RGB sao os bits primarios
 * (2/3 de intensidade) e r'g'b' os secundarios (1/3). Cada canal soma os dois,
 * dando 4 niveis: 0, 85, 170, 255.
 */
static void ega_color_to_rgb(uint8_t value, uint8_t rgb[3])
{
    uint8_t r = (uint8_t)((((value >> 2) & 1) << 1) | ((value >> 5) & 1));
    uint8_t g = (uint8_t)((((value >> 1) & 1) << 1) | ((value >> 4) & 1));
    uint8_t b = (uint8_t)((((value >> 0) & 1) << 1) | ((value >> 3) & 1));

    rgb[0] = (uint8_t)(r * 85);
    rgb[1] = (uint8_t)(g * 85);
    rgb[2] = (uint8_t)(b * 85);
}

void video_render_rgb(uint8_t *out)
{
    uint8_t lut[16][3];
    uint32_t base = ega.display_start;

    for (int i = 0; i < 16; i++) {
        ega_color_to_rgb(ega.palette[i], lut[i]);
    }

    for (uint32_t byte = 0; byte < EGA_PAGE_BYTES; byte++) {
        uint32_t addr = (base + byte) & (EGA_PLANE_SIZE - 1);
        uint8_t p0 = ega.vram[0][addr];
        uint8_t p1 = ega.vram[1][addr];
        uint8_t p2 = ega.vram[2][addr];
        uint8_t p3 = ega.vram[3][addr];

        /* O bit 7 e o pixel mais a esquerda. */
        for (int bit = 7; bit >= 0; bit--) {
            uint8_t index = (uint8_t)(
                  ((p0 >> bit) & 1)
                | (((p1 >> bit) & 1) << 1)
                | (((p2 >> bit) & 1) << 2)
                | (((p3 >> bit) & 1) << 3));

            *out++ = lut[index][0];
            *out++ = lut[index][1];
            *out++ = lut[index][2];
        }
    }
}

/* ------------------------------------------------------------------------- */
/* PNG                                                                       */
/* ------------------------------------------------------------------------- */

static void png_chunk(FILE *fp, const char *type, const uint8_t *data, size_t len)
{
    uint8_t be[4];
    uLong crc;

    be[0] = (uint8_t)(len >> 24); be[1] = (uint8_t)(len >> 16);
    be[2] = (uint8_t)(len >> 8);  be[3] = (uint8_t)len;
    fwrite(be, 1, 4, fp);

    fwrite(type, 1, 4, fp);
    if (len) fwrite(data, 1, len, fp);

    crc = crc32(0, (const Bytef *)type, 4);
    if (len) crc = crc32(crc, (const Bytef *)data, (uInt)len);
    be[0] = (uint8_t)(crc >> 24); be[1] = (uint8_t)(crc >> 16);
    be[2] = (uint8_t)(crc >> 8);  be[3] = (uint8_t)crc;
    fwrite(be, 1, 4, fp);
}

bool video_write_png(const char *path)
{
    static const uint8_t sig[8] = {137, 'P', 'N', 'G', '\r', '\n', 26, '\n'};
    uint8_t ihdr[13];
    uint8_t *rgb = NULL, *raw = NULL, *comp = NULL;
    uLongf complen;
    FILE *fp = NULL;
    bool ok = false;

    rgb = malloc((size_t)FB_PIXELS * 3);
    /* Cada scanline leva um byte de filtro (0 = None) na frente. */
    raw = malloc((size_t)EGA_SCREEN_H * (1 + EGA_SCREEN_W * 3));
    if (!rgb || !raw) goto done;

    video_render_rgb(rgb);

    for (int y = 0; y < EGA_SCREEN_H; y++) {
        uint8_t *dst = raw + (size_t)y * (1 + EGA_SCREEN_W * 3);
        *dst = 0;
        memcpy(dst + 1, rgb + (size_t)y * EGA_SCREEN_W * 3, EGA_SCREEN_W * 3);
    }

    complen = compressBound((uLong)EGA_SCREEN_H * (1 + EGA_SCREEN_W * 3));
    comp = malloc(complen);
    if (!comp) goto done;
    if (compress(comp, &complen, raw,
                 (uLong)EGA_SCREEN_H * (1 + EGA_SCREEN_W * 3)) != Z_OK) goto done;

    fp = fopen(path, "wb");
    if (!fp) goto done;

    fwrite(sig, 1, sizeof sig, fp);

    ihdr[0] = 0; ihdr[1] = 0;
    ihdr[2] = (uint8_t)(EGA_SCREEN_W >> 8); ihdr[3] = (uint8_t)EGA_SCREEN_W;
    ihdr[4] = 0; ihdr[5] = 0;
    ihdr[6] = (uint8_t)(EGA_SCREEN_H >> 8); ihdr[7] = (uint8_t)EGA_SCREEN_H;
    ihdr[8] = 8;    /* bits por canal */
    ihdr[9] = 2;    /* truecolor RGB */
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    png_chunk(fp, "IHDR", ihdr, sizeof ihdr);
    png_chunk(fp, "IDAT", comp, complen);
    png_chunk(fp, "IEND", NULL, 0);

    ok = true;

done:
    if (fp) fclose(fp);
    free(comp); free(raw); free(rgb);
    return ok;
}

/* ------------------------------------------------------------------------- */
/* Janela SDL                                                                */
/* ------------------------------------------------------------------------- */

bool video_init(const char *title, int scale)
{
    if (scale < 1) scale = 3;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }

    /* 320x200 nao e 4:3 -- o pixel do modo 0Dh e 20%% mais alto que largo.
     * Esticamos a altura para preservar as proporcoes originais. */
    int w = EGA_SCREEN_W * scale;
    int h = (int)(EGA_SCREEN_H * scale * 1.2);

    window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED, w, h,
                              SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return false;
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return false;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    SDL_RenderSetLogicalSize(renderer, w, h);

    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24,
                                SDL_TEXTUREACCESS_STREAMING,
                                EGA_SCREEN_W, EGA_SCREEN_H);
    if (!texture) {
        fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
        return false;
    }

    return true;
}

void video_present(void)
{
    uint8_t *pixels;
    int pitch;

    if (SDL_LockTexture(texture, NULL, (void **)&pixels, &pitch) != 0) return;

    if (pitch == EGA_SCREEN_W * 3) {
        video_render_rgb(pixels);
    } else {
        uint8_t *tmp = malloc((size_t)FB_PIXELS * 3);
        if (tmp) {
            video_render_rgb(tmp);
            for (int y = 0; y < EGA_SCREEN_H; y++) {
                memcpy(pixels + (size_t)y * pitch,
                       tmp + (size_t)y * EGA_SCREEN_W * 3, EGA_SCREEN_W * 3);
            }
            free(tmp);
        }
    }

    SDL_UnlockTexture(texture);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

bool video_pump_events(void)
{
    SDL_Event e;

    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return false;
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) return false;
    }
    return true;
}

void video_shutdown(void)
{
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
}
