/*
 * video.c -- Turn planar EGA memory into pixels and put them on screen.
 *
 * The EGA scatters each pixel across four planes: bit N of each plane forms
 * bit N of the palette index. This file de-interleaves that, runs the result
 * through the adapter's 6-bit palette, and hands it to SDL.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "cosmo/ega.h"
#include "cosmo/png.h"
#include "cosmo/video.h"

#define FB_PIXELS (EGA_SCREEN_W * EGA_SCREEN_H)
#define FB_BYTES  (FB_PIXELS * 3)

/*
 * Mode 0Dh pixels are not square: the 320x200 frame was displayed at 4:3, so
 * each pixel is 20% taller than it is wide. Stretching the height preserves
 * the proportions the artists actually drew for.
 */
#define PIXEL_ASPECT 1.2

static SDL_Window   *window;
static SDL_Renderer *renderer;
static SDL_Texture  *texture;

/*
 * An EGA color value holds six bits, r'g'b'RGB, where RGB are the primary bits
 * (two thirds intensity) and r'g'b' the secondary ones (one third). Each
 * channel sums both, giving four levels: 0, 85, 170, 255.
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

        /* Bit 7 is the leftmost pixel of the byte. */
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

bool video_write_png(const char *path, int scale)
{
    uint8_t *rgb = malloc(FB_BYTES);
    uint8_t *scaled = NULL;
    int out_w, out_h;
    bool ok = false;

    if (!rgb) return false;
    if (scale < 1) scale = 1;

    video_render_rgb(rgb);

    if (scale == 1) {
        ok = png_write_rgb(path, rgb, EGA_SCREEN_W, EGA_SCREEN_H);
        free(rgb);
        return ok;
    }

    out_w = EGA_SCREEN_W * scale;
    out_h = EGA_SCREEN_H * scale;
    scaled = malloc((size_t)out_w * (size_t)out_h * 3);
    if (!scaled) { free(rgb); return false; }

    for (int y = 0; y < out_h; y++) {
        const uint8_t *src = rgb + (size_t)(y / scale) * EGA_SCREEN_W * 3;
        uint8_t *dst = scaled + (size_t)y * (size_t)out_w * 3;

        for (int x = 0; x < out_w; x++) {
            const uint8_t *pixel = src + (size_t)(x / scale) * 3;
            *dst++ = pixel[0];
            *dst++ = pixel[1];
            *dst++ = pixel[2];
        }
    }

    ok = png_write_rgb(path, scaled, out_w, out_h);

    free(scaled);
    free(rgb);
    return ok;
}

bool video_init(const char *title, int scale)
{
    int w, h;

    if (scale < 1) scale = 3;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }

    w = EGA_SCREEN_W * scale;
    h = (int)(EGA_SCREEN_H * scale * PIXEL_ASPECT);

    if (!SDL_CreateWindowAndRenderer(title, w, h, SDL_WINDOW_RESIZABLE,
                                     &window, &renderer)) {
        fprintf(stderr, "SDL_CreateWindowAndRenderer: %s\n", SDL_GetError());
        return false;
    }

    /* Letterbox to the corrected aspect, so resizing never distorts. */
    SDL_SetRenderLogicalPresentation(renderer, w, h,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);

    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24,
                                SDL_TEXTUREACCESS_STREAMING,
                                EGA_SCREEN_W, EGA_SCREEN_H);
    if (!texture) {
        fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
        return false;
    }

    /* Chunky pixels, not a blurry upscale. */
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);

    return true;
}

void video_present(void)
{
    uint8_t *pixels;
    int pitch;

    if (!SDL_LockTexture(texture, NULL, (void **)&pixels, &pitch)) return;

    if (pitch == EGA_SCREEN_W * 3) {
        video_render_rgb(pixels);
    } else {
        uint8_t *tmp = malloc(FB_BYTES);
        if (tmp) {
            video_render_rgb(tmp);
            for (int y = 0; y < EGA_SCREEN_H; y++) {
                memcpy(pixels + (size_t)y * (size_t)pitch,
                       tmp + (size_t)y * EGA_SCREEN_W * 3,
                       EGA_SCREEN_W * 3);
            }
            free(tmp);
        }
    }

    SDL_UnlockTexture(texture);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

bool video_pump_events(void)
{
    SDL_Event e;

    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) return false;
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) return false;
    }
    return true;
}

void video_delay(uint32_t ms)
{
    SDL_Delay(ms);
}

void video_shutdown(void)
{
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
}
