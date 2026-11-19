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
#include "cosmo/mods.h"
#include "cosmo/png.h"
#include "cosmo/video.h"

#define FB_PIXELS (EGA_SCREEN_W * EGA_SCREEN_H)
#define FB_BYTES  (FB_PIXELS * 3)

/*
 * Mode 0Dh pixels are not square: the 320x200 frame was displayed at 4:3, so
 * each pixel is 20% taller than it is wide. Stretching the height preserves
 * the proportions the artists actually drew for.
 */
#define WIDE_PRESENT_W (EGA_SCREEN_W + EGA_WIDE_COLS * 8)
#define WIDE_WINDOW_H 243
#define WIDE_CONTENT_Y 1
#define CORRECTED_EGA_H (EGA_SCREEN_H * 6 / 5)

static int present_width = EGA_SCREEN_W;
static SDL_FRect wide_destination;

bool video_is_widescreen(void)
{
    return present_width == WIDE_PRESENT_W;
}

/* A restrained CRT look that stays inexpensive in the software renderer. */
static void apply_crt_shader(uint8_t *rgb, int width)
{
    static uint8_t softened[WIDE_PRESENT_W * EGA_SCREEN_H * 3];
    static const unsigned weights[3] = {1, 2, 1};

    /* A small separable Gaussian approximates the spread of phosphor light. */
    for (int y = 0; y < EGA_SCREEN_H; ++y) {
        for (int x = 0; x < width; ++x) {
            size_t dst = ((size_t)y * (size_t)width + (size_t)x) * 3;
            for (int c = 0; c < 3; ++c) {
                unsigned sum = 0;
                for (int tap = -1; tap <= 1; ++tap) {
                    int sx = x + tap;
                    if (sx < 0) sx = 0;
                    if (sx >= width) sx = width - 1;
                    size_t src = ((size_t)y * (size_t)width + (size_t)sx) * 3;
                    sum += rgb[src + (size_t)c] * weights[tap + 1];
                }
                softened[dst + (size_t)c] = (uint8_t)((sum + 2) / 4);
            }
        }
    }
    for (int y = 0; y < EGA_SCREEN_H; ++y) {
        for (int x = 0; x < width; ++x) {
            size_t dst = ((size_t)y * (size_t)width + (size_t)x) * 3;
            for (int c = 0; c < 3; ++c) {
                unsigned sum = 0;
                for (int tap = -1; tap <= 1; ++tap) {
                    int sy = y + tap;
                    if (sy < 0) sy = 0;
                    if (sy >= EGA_SCREEN_H) sy = EGA_SCREEN_H - 1;
                    size_t src = ((size_t)sy * (size_t)width + (size_t)x) * 3;
                    sum += softened[src + (size_t)c] * weights[tap + 1];
                }
                uint8_t filtered = (uint8_t)((sum + 2) / 4);
                rgb[dst + (size_t)c] = (uint8_t)(
                    ((unsigned)rgb[dst + (size_t)c] + filtered + 1) / 2);
            }
        }
    }
    for (int y = 0; y < EGA_SCREEN_H; ++y) {
        float ny = ((float)y + 0.5f) / (float)EGA_SCREEN_H * 2.0f - 1.0f;
        for (int x = 0; x < width; ++x) {
            float nx = ((float)x + 0.5f) / (float)width * 2.0f - 1.0f;
            float edge = 1.0f - 0.04f * (nx * nx + ny * ny);
            uint8_t *pixel = rgb + ((size_t)y * (size_t)width + (size_t)x) * 3;
            pixel[0] = (uint8_t)((float)pixel[0] * edge);
            pixel[1] = (uint8_t)((float)pixel[1] * edge);
            pixel[2] = (uint8_t)((float)pixel[2] * edge);
        }
    }
}

static void apply_crt_scanlines(uint8_t *rgb, int width)
{
    for (int y = 0; y < EGA_SCREEN_H; ++y) {
        float scanline = (y & 1) ? 0.93f : 1.0f;
        for (int x = 0; x < width; ++x) {
            uint8_t *pixel = rgb + ((size_t)y * (size_t)width + (size_t)x) * 3;
            float intensity = scanline;
            pixel[0] = (uint8_t)((float)pixel[0] * intensity);
            pixel[1] = (uint8_t)((float)pixel[1] * intensity);
            pixel[2] = (uint8_t)((float)pixel[2] * intensity);
        }
    }
}

static void apply_display_filters(uint8_t *rgb, int width)
{
    if (mods_crt_shader_enabled()) apply_crt_shader(rgb, width);
    if (mods_crt_scanlines_enabled()) apply_crt_scanlines(rgb, width);
}

static SDL_Window   *window;

SDL_Window *video_get_window(void)
{
    return window;
}
static SDL_Renderer *renderer;
static SDL_Texture  *texture;

static void load_palette(uint8_t lut[16][3])
{
    bool aurora = mods_feature_enabled(MOD_AURORA_PALETTE);

    for (int i = 0; i < 16; i++) {
        ega_palette_rgb(ega.palette[i], lut[i]);
        unsigned r = lut[i][0], g = lut[i][1], b = lut[i][2];
        if (aurora && (r != g || g != b)) {
            lut[i][0] = (uint8_t)SDL_min(255u, r + b / 4);
            lut[i][1] = (uint8_t)((g * 4 + b) / 5);
            lut[i][2] = (uint8_t)SDL_min(255u, b + r / 2 + g / 4);
        }
    }
}

static void render_ega_rgb(uint8_t *out, uint8_t lut[16][3])
{
    uint32_t base = ega.display_start;
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

void video_render_rgb(uint8_t *out)
{
    uint8_t lut[16][3];
    load_palette(lut);
    render_ega_rgb(out, lut);
    if (!video_is_widescreen()) apply_display_filters(out, EGA_SCREEN_W);
}

static void render_wide_hud_row(uint8_t *out, const uint8_t *src)
{
    /* Cuts in the original HUD artwork between the four counters. */
    static const unsigned cuts[] = {92, 157, 222};
    const unsigned extra = WIDE_PRESENT_W - EGA_SCREEN_W;
    const unsigned gaps = sizeof(cuts) / sizeof(cuts[0]);
    unsigned start = 0;
    unsigned added = 0;

    for (unsigned i = 0; i < gaps; ++i) {
        unsigned cut = cuts[i];
        unsigned gap = (i + 1) * extra / gaps - added;
        memcpy(out + (start + added) * 3, src + start * 3, (cut - start) * 3);

        /* Extend the wood and its trim using a column between counters. */
        for (unsigned x = 0; x < gap; ++x)
            memcpy(out + (cut + added + x) * 3, src + (cut - 1) * 3, 3);

        start = cut;
        added += gap;
    }
    memcpy(out + (start + added) * 3, src + start * 3, (EGA_SCREEN_W - start) * 3);
}

static void render_presentation(uint8_t *out)
{
    if (!video_is_widescreen()) {
        video_render_rgb(out);
        return;
    }

    static uint8_t classic[FB_BYTES];
    uint8_t lut[16][3];
    unsigned page = ega.display_start / EGA_PAGE_SIZE;
    const ega_wide_terrain *terrain = page < 2 && ega_wide_pages[page].visible
                                      ? &ega_wide_pages[page] : NULL;
    const unsigned centered_left = (WIDE_PRESENT_W - EGA_SCREEN_W) / 2;
    const unsigned playfield_end = (1 + EGA_WIDE_ROWS) * 8;
    load_palette(lut);
    render_ega_rgb(classic, lut);
    memset(out, 0, WIDE_PRESENT_W * EGA_SCREEN_H * 3);

    /* Keep the EGA rows intact until the frame is scaled for display. */
    for (unsigned y = 0; y < EGA_SCREEN_H; ++y) {
        uint8_t *row = out + y * WIDE_PRESENT_W * 3;
        const uint8_t *source_row = classic + y * EGA_SCREEN_W * 3;
        if (terrain && y >= playfield_end) {
            render_wide_hud_row(row, source_row);
            continue;
        }

        unsigned left = terrain ? 0 : centered_left;
        memcpy(row + left * 3, source_row, EGA_SCREEN_W * 3);
        if (!terrain || y < 8) continue;
        for (unsigned x = 0; x < EGA_WIDE_COLS * 8; ++x) {
            unsigned addr = (y - 8) * EGA_WIDE_COLS + x / 8;
            unsigned bit = 0x80u >> (x % 8), index = 0;
            for (unsigned plane = 0; plane < EGA_PLANES; ++plane)
                if (terrain->planes[plane][addr] & bit) index |= 1u << plane;
            /* The last byte of each original playfield scanline is its border. */
            memcpy(row + (EGA_WIDE_FIRST_COLUMN * 8 + x) * 3, lut[index], 3);
        }
    }

    apply_display_filters(out, WIDE_PRESENT_W);
}

bool video_write_png(const char *path, int scale)
{
    bool widescreen = video_is_widescreen();
    uint8_t *rgb = malloc((size_t)present_width * EGA_SCREEN_H * 3);
    uint8_t *scaled = NULL;
    int out_w, out_h;
    int content_y, content_h;
    bool ok = false;

    if (!rgb) return false;
    if (scale < 1) scale = 1;

    render_presentation(rgb);

    if (!widescreen && scale == 1) {
        ok = png_write_rgb(path, rgb, present_width, EGA_SCREEN_H);
        free(rgb);
        return ok;
    }
    out_w = present_width * scale;
    out_h = (widescreen ? WIDE_WINDOW_H : EGA_SCREEN_H) * scale;
    content_y = widescreen ? WIDE_CONTENT_Y * scale : 0;
    content_h = (widescreen ? CORRECTED_EGA_H : EGA_SCREEN_H) * scale;

    scaled = calloc((size_t)out_h, (size_t)out_w * 3);
    if (!scaled) { free(rgb); return false; }

    for (int y = 0; y < content_h; y++) {
        size_t source_y = (size_t)y * EGA_SCREEN_H / (size_t)content_h;
        const uint8_t *src = rgb + source_y * (size_t)present_width * 3;
        uint8_t *dst = scaled + (size_t)(y + content_y) * (size_t)out_w * 3;

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

/*
 * The largest whole-number scale that leaves the window on the screen.
 *
 * Three times 640x400, corrected for the aspect the pixels had, is 1920x1440 --
 * taller than a 1080p display, which puts the title bar above the top of the
 * screen where it cannot be reached. The scale is chosen to fit instead, and
 * COSMO_SCALE overrides it for anyone who wants a particular size.
 */
#define WINDOW_FRAME_ALLOWANCE 64

static int fitting_scale(int wanted, int width, int height)
{
    const char *requested = SDL_getenv("COSMO_SCALE");
    SDL_Rect usable;
    SDL_DisplayID display;
    int scale;

    if (requested) {
        int value = SDL_atoi(requested);
        if (value >= 1) return value;
    }

    display = SDL_GetPrimaryDisplay();
    if (!display || !SDL_GetDisplayUsableBounds(display, &usable)) return wanted;

    for (scale = wanted; scale > 1; scale--) {
        int w = width * scale;
        int h = height * scale;

        /*
         * The usable bounds already exclude the taskbar or menu bar; what they
         * do not account for is the window's own title bar, which is what ends
         * up off the top of the screen when the window is too tall.
         */
        if (w <= usable.w && h <= usable.h - WINDOW_FRAME_ALLOWANCE) break;
    }

    return scale;
}

bool video_init(const char *title, int scale, bool widescreen)
{
    int width = widescreen ? WIDE_PRESENT_W : EGA_SCREEN_W;
    int height = widescreen ? WIDE_WINDOW_H : CORRECTED_EGA_H;

    if (scale < 1) scale = 3;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }

    scale = fitting_scale(scale, width, height);

    int w = width * scale;
    int h = height * scale;

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
                                width, EGA_SCREEN_H);
    if (!texture) {
        fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
        return false;
    }
    present_width = width;
    if (widescreen) {
        wide_destination = (SDL_FRect){
            0, WIDE_CONTENT_Y * scale, w, CORRECTED_EGA_H * scale
        };
    }

    /* Chunky pixels, not a blurry upscale. */
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);

    /* A launcher-to-episode handoff creates a fresh SDL window in the same
     * process. Restore and raise it explicitly so it does not stay minimized
     * or behind other windows after that handoff. */
    SDL_RestoreWindow(window);
    SDL_RaiseWindow(window);

    return true;
}

void video_present(void)
{
    uint8_t *pixels;
    int pitch;
    int row_bytes = present_width * 3;

    if (!SDL_LockTexture(texture, NULL, (void **)&pixels, &pitch)) return;

    if (pitch == row_bytes) {
        render_presentation(pixels);
    } else {
        uint8_t *tmp = malloc((size_t)row_bytes * EGA_SCREEN_H);
        if (tmp) {
            render_presentation(tmp);
            for (int y = 0; y < EGA_SCREEN_H; y++) {
                memcpy(pixels + (size_t)y * (size_t)pitch,
                       tmp + (size_t)y * (size_t)row_bytes,
                       (size_t)row_bytes);
            }
            free(tmp);
        }
    }

    SDL_UnlockTexture(texture);
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, NULL,
                      video_is_widescreen() ? &wide_destination : NULL);
    SDL_RenderPresent(renderer);
}

bool video_is_fullscreen(void)
{
    return window && (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN);
}

void video_set_fullscreen(bool fullscreen)
{
    if (!SDL_SetWindowFullscreen(window, fullscreen) || !SDL_SyncWindow(window)) {
        SDL_Log("Could not change fullscreen mode: %s", SDL_GetError());
    }
}

bool video_poll_event(SDL_Event *event)
{
    while (SDL_PollEvent(event)) {
        if (event->type == SDL_EVENT_KEY_DOWN) {
            const SDL_KeyboardEvent *key = &event->key;
            bool enter = key->scancode == SDL_SCANCODE_RETURN ||
                         key->scancode == SDL_SCANCODE_KP_ENTER;

            if (key->scancode == SDL_SCANCODE_F11 ||
                (enter && (key->mod & SDL_KMOD_ALT))) {
                if (!key->repeat) {
                    video_set_fullscreen(!video_is_fullscreen());
                    video_present();
                }
                continue;
            }
        }
        return true;
    }
    return false;
}

video_key video_poll_key(void)
{
    SDL_Event e;

    while (video_poll_event(&e)) {
        if (e.type == SDL_EVENT_QUIT) return VIDEO_KEY_QUIT;

        if (e.type == SDL_EVENT_KEY_DOWN) {
            switch (e.key.key) {
            case SDLK_ESCAPE:
            case SDLK_Q:      return VIDEO_KEY_QUIT;
            case SDLK_RIGHT:
            case SDLK_SPACE:  return VIDEO_KEY_NEXT;
            case SDLK_LEFT:   return VIDEO_KEY_PREV;
            case SDLK_S:      return VIDEO_KEY_SCREENSHOT;
            default: break;
            }
        }
    }
    return VIDEO_KEY_NONE;
}

void video_set_title(const char *title)
{
    if (window) SDL_SetWindowTitle(window, title);
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
    present_width = EGA_SCREEN_W;
    SDL_Quit();
}
