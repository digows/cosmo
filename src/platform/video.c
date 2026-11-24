/*
 * video.c -- Turn planar EGA memory into pixels and put them on screen.
 *
 * The EGA scatters each pixel across four planes: bit N of each plane forms
 * bit N of the palette index. This file de-interleaves that, runs the result
 * through the adapter's 6-bit palette, and hands it to SDL.
 */

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <math.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "cosmo/ega.h"
#include "cosmo/mods.h"
#include "cosmo/png.h"
#include "cosmo/video.h"
#include "cosmo/font.h"

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
#define MOTION_FRAME_BYTES (WIDE_PRESENT_W * EGA_SCREEN_H * 3)
#define MOTION_BLOCK 8
#define MOTION_SEARCH 12
#define MOTION_BLOCKS_X ((WIDE_PRESENT_W + MOTION_BLOCK - 1) / MOTION_BLOCK)
#define MOTION_BLOCKS_Y ((EGA_SCREEN_H + MOTION_BLOCK - 1) / MOTION_BLOCK)
#define MOTION_WORLD_TOP 8
#define MOTION_WORLD_BOTTOM 152
#define MOTION_BACKGROUND_BANDS 3

static int present_width = EGA_SCREEN_W;
static SDL_FRect wide_destination;
static SDL_Mutex *motion_mutex;
static bool motion_interpolation_enabled;
static bool pixel_camera_preview;
static bool steady_camera_preview;
static uint64_t preview_frame_period, preview_frame_start;
/* Original unshifted backdrop, stored as 40 by 18 EGA tiles. */
enum { PREVIEW_BACKDROP_W = 320, PREVIEW_BACKDROP_H = 144,
       PREVIEW_BACKDROP_BASE = 0xa300, PREVIEW_MASK_STRIDE = WIDE_PRESENT_W / 8 };
static uint8_t preview_page_mask[2][EGA_SCREEN_H][PREVIEW_MASK_STRIDE];
static uint8_t preview_previous_mask[EGA_SCREEN_H][PREVIEW_MASK_STRIDE];
static uint8_t preview_current_mask[EGA_SCREEN_H][PREVIEW_MASK_STRIDE];
static uint8_t preview_backdrop[PREVIEW_BACKDROP_H][PREVIEW_BACKDROP_W][3];
static bool preview_backdrop_x, preview_backdrop_y, preview_layer_valid;
/* The outer poses can spread rings across more than 64 source pixels. */
enum { BLAST_SIDE = 128, BLAST_RING_LIMIT = 24 };
typedef struct {
    int x, y;
} blast_ring;

typedef struct {
    bool visible, tracked, spin, expand, in_front, edge_padding, slow_rise;
    uint32_t identity;
    int x, y;
    int left, right, top, bottom;
    uint8_t pixels[EGA_SCREEN_H][WIDE_PRESENT_W];
    uint8_t occlusion[EGA_SCREEN_H][PREVIEW_MASK_STRIDE];
    unsigned ring_count;
    blast_ring rings[BLAST_RING_LIMIT];
    uint8_t ring_labels[BLAST_SIDE][BLAST_SIDE];
} preview_player;
static bool preview_player_enabled;
static preview_player preview_player_pages[2], preview_current_player;
static bool preview_previous_player_tracked;
static int preview_previous_player_x, preview_previous_player_y;
static uint8_t preview_player_palette[16][3];
static bool preview_star_enabled;
static bool smooth_gameplay_enabled;
typedef struct {
    preview_player *sprites;
    size_t count, capacity;
} preview_star_list;
static preview_star_list preview_star_pages[2], preview_stars, preview_previous_stars;
static uint8_t preview_star_terrain[2][EGA_SCREEN_H][PREVIEW_MASK_STRIDE];

static bool reserve_preview_stars(preview_star_list *list, size_t count)
{
    if (list->capacity >= count) return true;
    size_t capacity = SDL_max(count, SDL_max(8, list->capacity * 2));
    preview_player *sprites = SDL_realloc(list->sprites, capacity * sizeof *sprites);
    if (!sprites) return false;
    list->sprites = sprites;
    list->capacity = capacity;
    return true;
}

/* Each ring is an opaque island in the original transparent blast sprite. */
static void identify_blast_rings(preview_player *sprite)
{
    int width = sprite->right - sprite->left;
    int height = sprite->bottom - sprite->top;
    if (!sprite->expand || width > BLAST_SIDE || height > BLAST_SIDE) return;
    uint16_t queue[BLAST_SIDE * BLAST_SIDE];
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (sprite->ring_labels[y][x] || !sprite->pixels[sprite->top + y][sprite->left + x])
                continue;
            if (sprite->ring_count == BLAST_RING_LIMIT) return;
            unsigned label = ++sprite->ring_count;
            unsigned read = 0, write = 0, sum_x = 0, sum_y = 0;
            queue[write++] = (uint16_t)(y * BLAST_SIDE + x);
            sprite->ring_labels[y][x] = (uint8_t)label;
            while (read < write) {
                unsigned point = queue[read++];
                int px = point % BLAST_SIDE, py = point / BLAST_SIDE;
                sum_x += px;
                sum_y += py;
                for (int ny = SDL_max(0, py - 1); ny <= SDL_min(height - 1, py + 1); ++ny) {
                    for (int nx = SDL_max(0, px - 1); nx <= SDL_min(width - 1, px + 1); ++nx) {
                        if (sprite->ring_labels[ny][nx] ||
                            !sprite->pixels[sprite->top + ny][sprite->left + nx]) continue;
                        sprite->ring_labels[ny][nx] = (uint8_t)label;
                        queue[write++] = (uint16_t)(ny * BLAST_SIDE + nx);
                    }
                }
            }
            sprite->rings[label - 1] = (blast_ring){
                sprite->left + (int)((sum_x + write / 2) / write),
                sprite->top + (int)((sum_y + write / 2) / write)
            };
        }
    }
}
static unsigned motion_interpolation_pause_depth;
static bool motion_vectors_valid;
static uint8_t motion_capture[MOTION_FRAME_BYTES];
static uint8_t motion_previous[MOTION_FRAME_BYTES];
static uint8_t motion_current[MOTION_FRAME_BYTES];
static uint8_t motion_output[MOTION_FRAME_BYTES];
static int8_t motion_dx[MOTION_BLOCKS_X * MOTION_BLOCKS_Y];
static int8_t motion_dy[MOTION_BLOCKS_X * MOTION_BLOCKS_Y];
static uint8_t motion_confident[MOTION_BLOCKS_X * MOTION_BLOCKS_Y];
static int8_t motion_filtered_dx[MOTION_BLOCKS_X * MOTION_BLOCKS_Y];
static int8_t motion_filtered_dy[MOTION_BLOCKS_X * MOTION_BLOCKS_Y];
static uint8_t motion_filtered_confident[MOTION_BLOCKS_X * MOTION_BLOCKS_Y];
static unsigned motion_frame_count;
static uint64_t motion_previous_time, motion_current_time;
static uint16_t motion_previous_camera_x, motion_previous_camera_y;
static uint16_t motion_current_camera_x, motion_current_camera_y;
static bool motion_camera_valid;

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
static SDL_Texture *launcher_touch_texture;
static bool touch_controls;
static unsigned touch_context;
static SDL_AtomicInt touch_hud_enabled;
static unsigned touch_pressed;
enum { TOUCH_SPRITE_SIDE = 32 };
static uint8_t touch_bomb_pixels[TOUCH_SPRITE_SIDE][TOUCH_SPRITE_SIDE];
static unsigned touch_bomb_left, touch_bomb_top, touch_bomb_right, touch_bomb_bottom;
static SDL_AtomicInt touch_bomb_ready;
enum { TOUCH_GUTTER = 80, TOUCH_CANVAS_W = EGA_SCREEN_W + 2 * TOUCH_GUTTER };
static const SDL_FRect touch_buttons[TOUCH_BUTTON_COUNT] = {
    [TOUCH_LEFT]  = {  7, 129, 32, 32},
    [TOUCH_RIGHT] = { 41, 129, 32, 32},
    [TOUCH_UP]    = { 24,  95, 32, 32},
    [TOUCH_DOWN]  = { 24, 163, 32, 32},
    [TOUCH_JUMP]  = {416, 154, 56, 46},
    [TOUCH_BOMB]  = {427,  88, 46, 46},
    [TOUCH_PAUSE] = {436,  18, 36, 34},
};
static const SDL_FRect hud_touch_buttons[TOUCH_BUTTON_COUNT] = {
    [TOUCH_LEFT]  = { 13, 191, 31, 43},
    [TOUCH_RIGHT] = { 73, 191, 31, 43},
    [TOUCH_UP]    = { 49, 191, 19, 20},
    [TOUCH_DOWN]  = { 49, 214, 19, 20},
    [TOUCH_JUMP]  = {374, 191, 44, 43},
    [TOUCH_BOMB]  = {340, 208, 33, 26},
    [TOUCH_PAUSE] = {340, 189, 33, 16},
};

void video_set_touch_controls(bool enabled)
{
    if (touch_controls == enabled) return;
    touch_controls = enabled;
    if (video_is_widescreen()) {
        SDL_SetAtomicInt(&touch_hud_enabled, enabled);
        int scale = enabled ? 1 : (int)wide_destination.w / WIDE_PRESENT_W;
        SDL_SetRenderLogicalPresentation(renderer, WIDE_PRESENT_W * scale,
            WIDE_WINDOW_H * scale, SDL_LOGICAL_PRESENTATION_LETTERBOX);
        return;
    }
    SDL_SetRenderLogicalPresentation(renderer,
        enabled ? TOUCH_CANVAS_W : EGA_SCREEN_W, CORRECTED_EGA_H,
        SDL_LOGICAL_PRESENTATION_LETTERBOX);
}

void video_set_touch_pressed(unsigned buttons)
{
    touch_pressed = buttons;
}

void video_set_touch_context(unsigned context)
{
    touch_context = context;
}

void video_set_touch_bomb_sprite(const uint8_t *tiles, unsigned columns, unsigned rows)
{
    if (!columns || columns > TOUCH_SPRITE_SIDE / 8 ||
        !rows || rows > TOUCH_SPRITE_SIDE / 8) return;
    touch_bomb_left = columns * 8;
    touch_bomb_top = rows * 8;
    touch_bomb_right = touch_bomb_bottom = 0;
    for (unsigned ty = 0; ty < rows; ++ty) {
        for (unsigned tx = 0; tx < columns; ++tx, tiles += 40) {
            for (unsigned y = 0; y < 8; ++y) {
                const uint8_t *planes = tiles + y * 5;
                for (unsigned x = 0; x < 8; ++x) {
                    unsigned bit = 0x80u >> x;
                    unsigned color = 0;
                    for (unsigned plane = 0; plane < EGA_PLANES; ++plane)
                        if (planes[plane + 1] & bit) color |= 1u << plane;
                    unsigned px = tx * 8 + x, py = ty * 8 + y;
                    touch_bomb_pixels[py][px] = (planes[0] & bit) ? 0 : color + 1;
                    if (!(planes[0] & bit)) {
                        touch_bomb_left = SDL_min(touch_bomb_left, px);
                        touch_bomb_top = SDL_min(touch_bomb_top, py);
                        touch_bomb_right = SDL_max(touch_bomb_right, px + 1);
                        touch_bomb_bottom = SDL_max(touch_bomb_bottom, py + 1);
                    }
                }
            }
        }
    }
    SDL_SetAtomicInt(&touch_bomb_ready, touch_bomb_right > touch_bomb_left);
}

static bool touch_button_visible(int button)
{
    return touch_context == TOUCH_PLAY || (touch_context == TOUCH_MENU && button != TOUCH_PAUSE);
}

static SDL_FRect touch_button_rect(int button)
{
    if (!video_is_widescreen()) return touch_buttons[button];
    if (touch_context == TOUCH_MENU && button == TOUCH_BOMB)
        return (SDL_FRect){329, 201, 44, 33};
    return hud_touch_buttons[button];
}

int video_touch_button(SDL_Event *event)
{
    if (!touch_controls || !SDL_ConvertEventToRenderCoordinates(renderer, event))
        return -1;
    SDL_FPoint point = {event->tfinger.x, event->tfinger.y};
    for (int i = 0; i < TOUCH_BUTTON_COUNT; ++i) {
        SDL_FRect button = touch_button_rect(i);
        if (touch_button_visible(i) && SDL_PointInRectFloat(&point, &button)) return i;
    }
    return -1;
}

static void touch_rect(float x, float y, float w, float h, uint32_t rgb)
{
    SDL_FRect rect = {x, y, w, h};
    SDL_SetRenderDrawColor(renderer, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8),
                           (Uint8)rgb, 255);
    SDL_RenderFillRect(renderer, &rect);
}

void video_load_launcher_touch_texture(void)
{
    SDL_Surface *art = SDL_LoadPNG("launcher-touch-panel.png");
    if (!art) {
        SDL_Log("Touch panel artwork unavailable: %s", SDL_GetError());
        return;
    }
    launcher_touch_texture = SDL_CreateTextureFromSurface(renderer, art);
    SDL_DestroySurface(art);
    if (launcher_touch_texture)
        SDL_SetTextureScaleMode(launcher_touch_texture, SDL_SCALEMODE_NEAREST);
}

static void draw_launcher_touch_panel(int x)
{
    SDL_FRect panel = {x + 1, 1, TOUCH_GUTTER - 2, CORRECTED_EGA_H - 2};
    if (launcher_touch_texture)
        SDL_RenderTexture(renderer, launcher_touch_texture, NULL, &panel);
    else
        touch_rect(panel.x, panel.y, panel.w, panel.h, 0xaa5500);
}

static void draw_touch_bomb(SDL_FRect button, bool pressed)
{
    float x = SDL_floorf(button.x + (button.w - touch_bomb_right + touch_bomb_left) / 2);
    float y = SDL_floorf(button.y + (button.h - touch_bomb_bottom + touch_bomb_top) / 2) + pressed;
    for (unsigned row = touch_bomb_top; row < touch_bomb_bottom; ++row) {
        for (unsigned col = touch_bomb_left; col < touch_bomb_right; ++col) {
            unsigned index = touch_bomb_pixels[row][col];
            if (!index) continue;
            --index;
            uint8_t rgb[3];
            ega_palette_rgb(index < 8 ? index : index + 8, rgb);
            touch_rect(x + col - touch_bomb_left, y + row - touch_bomb_top, 1, 1,
                       ((uint32_t)rgb[0] << 16) | ((uint32_t)rgb[1] << 8) | rgb[2]);
        }
    }
}

static void draw_touch_glyph(const uint8_t rows[8], float x, float y, float pixel,
                             bool bevel)
{
    float shadow = SDL_max(1.0f, pixel * 0.5f);
    for (int row = 0; row < 8; ++row)
        for (int col = 0; col < 8; ++col)
            if (rows[row] & (0x80u >> col))
                touch_rect(x + col * pixel - shadow, y + row * pixel + shadow,
                           pixel, pixel, 0x000000);

    int detail = bevel && pixel >= 1.5f ? 2 : 1;
    float unit = pixel / detail;
    for (int row = 0; row < 8 * detail; ++row) {
        for (int col = 0; col < 8 * detail; ++col) {
            int glyph_row = row / detail;
            unsigned bit = 0x80u >> (col / detail);
            if (!(rows[glyph_row] & bit)) continue;
            uint32_t ink = 0x55ffff;
            if (bevel) {
                bool top = row % detail == 0 &&
                    (glyph_row == 0 || !(rows[glyph_row - 1] & bit));
                if (top) ink = 0x62ffff;
                else if (row < 4 * detail) ink = 0x00c6cd;
                else if (row < 6 * detail) ink = ((row + col) & 1) ? 0x006b70 : 0x00b9be;
                else ink = 0x007c82;
            }
            touch_rect(x + col * unit, y + row * unit, unit, unit, ink);
        }
    }
}

static void draw_touch_frame(SDL_FRect button, bool pressed)
{
    /* The score well's stepped corners, grey fill and down-left shadow. */
    static const char *const rows[] = {
        "wwrrrryww",
        "wkwwwwyww",
        "kwwkkkkyw",
        "kwrgggkrw",
        "kwwrrrrrw",
        "wkwwwwrww",
        "wwkkkkwww",
    };
    const uint32_t palette[] = {0xaa5500, 0x000000, 0xff5555, 0xffff55,
                               pressed ? 0x343434 : 0x555555};
    float y = button.y;
    for (unsigned row = 0; row < SDL_arraysize(rows); ++row) {
        float height = row == 3 ? button.h - 6 : 1;
        float x = button.x;
        for (unsigned col = 0; col < 9; ++col) {
            float width = col == 4 ? button.w - 8 : 1;
            char color = rows[row][col];
            uint32_t rgb = color == 'w' ? palette[0] : color == 'k' ? palette[1] :
                           color == 'r' ? palette[2] :
                           color == 'y' ? palette[3] : palette[4];
            touch_rect(x, y, width, height, rgb);
            x += width;
        }
        y += height;
    }
}

static void draw_touch_label(const char *label, SDL_FRect button, bool pressed, bool bevel)
{
    size_t length = SDL_strlen(label);
    float preferred_pixel = length <= 2 ? 1.5f : 1;
    float pixel = SDL_min(preferred_pixel, (button.w - 8) / (length * 8));
    float x = SDL_floorf(button.x + (button.w - length * 8 * pixel) / 2);
    float y = SDL_floorf(button.y + (button.h - 8 * pixel) / 2) + pressed;
    for (size_t i = 0; i < length; ++i) {
        const uint8_t *tile = font_glyph(label[i]);
        if (!tile) continue;
        uint8_t rows[8];
        for (unsigned row = 0; row < 8; ++row)
            rows[row] = tile[row * 5 + 1] & tile[row * 5 + 2] &
                        tile[row * 5 + 3] & tile[row * 5 + 4];
        draw_touch_glyph(rows, x + i * 8 * pixel, y, pixel, bevel);
    }
}

static void render_touch_controls(bool hud)
{
    if (!hud) {
        draw_launcher_touch_panel(0);
        draw_launcher_touch_panel(TOUCH_GUTTER + EGA_SCREEN_W);
    }

    static const uint8_t icons[TOUCH_BUTTON_COUNT][8] = {
        {0x10, 0x30, 0x70, 0xfe, 0xfe, 0x70, 0x30, 0x10},
        {0x08, 0x0c, 0x0e, 0x7f, 0x7f, 0x0e, 0x0c, 0x08},
        {0, 0x18, 0x3c, 0x7e, 0x18, 0x18, 0x18, 0},
        {0, 0x18, 0x18, 0x18, 0x7e, 0x3c, 0x18, 0},
        {0, 0x18, 0x3c, 0x7e, 0x18, 0x18, 0x18, 0},
        {0x03, 0x04, 0x18, 0x3c, 0x7e, 0x7e, 0x7e, 0x3c},
        {0, 0, 0x66, 0x66, 0x66, 0x66, 0, 0},
    };
    for (int i = 0; i < TOUCH_BUTTON_COUNT; ++i) {
        if (!touch_button_visible(i)) continue;
        SDL_FRect r = touch_button_rect(i);
        bool down = (touch_pressed & (1u << i)) != 0;
        draw_touch_frame(r, down);
        if (touch_context == TOUCH_MENU && (i == TOUCH_JUMP || i == TOUCH_BOMB)) {
            draw_touch_label(i == TOUCH_JUMP ? "SELECT" : "BACK", r, down, false);
            continue;
        }
        if (i == TOUCH_PAUSE && font_glyph('P')) {
            draw_touch_label("PAUSE", r, down, true);
            continue;
        }
        if (i == TOUCH_BOMB && SDL_GetAtomicInt(&touch_bomb_ready)) {
            draw_touch_bomb(r, down);
            continue;
        }
        float pixel = i == TOUCH_JUMP ? 4 : (hud && SDL_min(r.w, r.h) <= 21) ? 1.5f : 3;
        pixel = SDL_min(pixel, (SDL_min(r.w, r.h) - 10) / 8);
        float x = r.x + (r.w - 8 * pixel) / 2;
        if (i == TOUCH_LEFT) x += pixel / 2;
        if (i == TOUCH_RIGHT) x -= pixel / 2;
        float y = r.y + (r.h - 8 * pixel) / 2 + (down ? 2 : 0);
        draw_touch_glyph(icons[i], x, y, pixel, i <= TOUCH_JUMP);
    }
}
static bool renderer_vsync;
static double display_refresh_hz = 60.0;
static uint64_t next_manual_present;

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

static void render_touch_hud_row(uint8_t *out, const uint8_t *src, unsigned y)
{
    /* Copy the original 320-pixel HUD artwork without resampling it. */
    static const struct {
        unsigned source_x, width, dest_x;
    } counters[] = {
        { 19, 65, 109 }, /* Score */
        { 97, 54, 178 }, /* Health */
        {173, 44, 236 }, /* Bombs */
        {239, 53, 284 }, /* Stars */
    };
    const unsigned trim = 12;
    memcpy(out, src, trim * 3);
    memcpy(out + (WIDE_PRESENT_W - trim) * 3,
           src + (EGA_SCREEN_W - trim) * 3, trim * 3);
    for (unsigned x = trim; x < WIDE_PRESENT_W - trim; ++x)
        memcpy(out + x * 3, src + 91 * 3, 3);
    for (unsigned i = y >= 188 && y < 197 ? 1 : 0; i < SDL_arraysize(counters); ++i)
        memcpy(out + counters[i].dest_x * 3,
               src + counters[i].source_x * 3, counters[i].width * 3);
}

static void render_wide_hud_row(uint8_t *out, const uint8_t *src, unsigned y)
{
    if (SDL_GetAtomicInt(&touch_hud_enabled)) {
        render_touch_hud_row(out, src, y);
        return;
    }
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

static void render_presentation(uint8_t *out, bool filters)
{
    if (!video_is_widescreen()) {
        uint8_t lut[16][3];
        load_palette(lut);
        render_ega_rgb(out, lut);
        if (filters) apply_display_filters(out, EGA_SCREEN_W);
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
            render_wide_hud_row(row, source_row, y);
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

    if (filters) apply_display_filters(out, WIDE_PRESENT_W);
}

static unsigned motion_luma(const uint8_t *pixel)
{
    return ((unsigned)pixel[0] * 3u + (unsigned)pixel[1] * 6u + pixel[2]) / 10u;
}

static unsigned motion_block_cost(const uint8_t *previous,
                                  const uint8_t *current, int width,
                                  int x, int y, int ox, int oy)
{
    unsigned cost = 0, samples = 0;
    for (int sy = 1; sy < MOTION_BLOCK; sy += 2) {
        int cy = y + sy;
        int py = cy + oy;
        if (cy >= EGA_SCREEN_H || py < 0 || py >= EGA_SCREEN_H) continue;
        for (int sx = 1; sx < MOTION_BLOCK; sx += 2) {
            int cx = x + sx;
            int px = cx + ox;
            const uint8_t *a, *b;
            unsigned la, lb;
            if (cx >= width || px < 0 || px >= width) continue;
            a = current + ((size_t)cy * (size_t)width + (size_t)cx) * 3;
            b = previous + ((size_t)py * (size_t)width + (size_t)px) * 3;
            la = motion_luma(a);
            lb = motion_luma(b);
            cost += la > lb ? la - lb : lb - la;
            ++samples;
        }
    }
    return samples ? cost / samples : UINT_MAX;
}

static void estimate_motion_vectors(const uint8_t *previous,
                                    const uint8_t *current, int width)
{
    unsigned boundary_vectors = 0, total_vectors = 0, confident_vectors = 0;
    memset(motion_dx, 0, sizeof motion_dx);
    memset(motion_dy, 0, sizeof motion_dy);
    memset(motion_confident, 0, sizeof motion_confident);

    for (int y = MOTION_WORLD_TOP; y < MOTION_WORLD_BOTTOM; y += MOTION_BLOCK) {
        for (int x = 0; x < width; x += MOTION_BLOCK) {
            unsigned stationary = motion_block_cost(previous, current, width, x, y, 0, 0);
            unsigned best_cost = stationary, second_cost = UINT_MAX;
            int best_x = 0, best_y = 0;
            int block = (y / MOTION_BLOCK) * MOTION_BLOCKS_X + x / MOTION_BLOCK;

            for (int oy = -MOTION_SEARCH; oy <= MOTION_SEARCH; oy += 2) {
                for (int ox = -MOTION_SEARCH; ox <= MOTION_SEARCH; ox += 2) {
                    unsigned cost;
                    if (ox == 0 && oy == 0) continue;
                    cost = motion_block_cost(previous, current, width, x, y, ox, oy);
                    if (cost < best_cost) {
                        second_cost = best_cost;
                        best_cost = cost;
                        best_x = ox;
                        best_y = oy;
                    } else if (cost < second_cost) {
                        second_cost = cost;
                    }
                }
            }
            {
                int center_x = best_x, center_y = best_y;
                for (int oy = center_y - 1; oy <= center_y + 1; ++oy) {
                    for (int ox = center_x - 1; ox <= center_x + 1; ++ox) {
                        unsigned cost;
                        if (ox < -MOTION_SEARCH || ox > MOTION_SEARCH ||
                            oy < -MOTION_SEARCH || oy > MOTION_SEARCH) continue;
                        cost = motion_block_cost(previous, current, width, x, y, ox, oy);
                        if (cost < best_cost) {
                            second_cost = best_cost;
                            best_cost = cost;
                            best_x = ox;
                            best_y = oy;
                        } else if (cost < second_cost) {
                            second_cost = cost;
                        }
                    }
                }
            }

            /* Repetitive scenery often has several equally plausible matches. */
            if ((best_x != 0 || best_y != 0) &&
                best_cost + 3u < stationary && best_cost + 5u < second_cost) {
                int old_x = x + best_x, old_y = y + best_y;
                int expected_x = -best_x, expected_y = -best_y;
                int reverse_x = expected_x, reverse_y = expected_y;
                unsigned reverse_cost = UINT_MAX;

                /* A good current->previous match should point back to current. */
                for (int ry = expected_y - 2; ry <= expected_y + 2; ++ry) {
                    for (int rx = expected_x - 2; rx <= expected_x + 2; ++rx) {
                        unsigned cost;
                        if (rx < -MOTION_SEARCH || rx > MOTION_SEARCH ||
                            ry < -MOTION_SEARCH || ry > MOTION_SEARCH) continue;
                        cost = motion_block_cost(current, previous, width,
                                                 old_x, old_y, rx, ry);
                        if (cost < reverse_cost) {
                            reverse_cost = cost;
                            reverse_x = rx;
                            reverse_y = ry;
                        }
                    }
                }

                if (abs(reverse_x - expected_x) <= 1 &&
                    abs(reverse_y - expected_y) <= 1 &&
                    reverse_cost <= best_cost + 6u) {
                    motion_dx[block] = (int8_t)-best_x;
                    motion_dy[block] = (int8_t)-best_y;
                    motion_confident[block] = 1;
                    ++confident_vectors;
                    if (abs(best_x) == MOTION_SEARCH || abs(best_y) == MOTION_SEARCH)
                        ++boundary_vectors;
                }
            }
            ++total_vectors;
        }
    }

    /* Large camera snaps cannot be inferred reliably from a short search. */
    motion_vectors_valid = total_vectors != 0 && confident_vectors > 2 &&
                           boundary_vectors * 3 < total_vectors;

    /* Remove isolated matches, a common failure on repeated tile patterns. */
    memcpy(motion_filtered_dx, motion_dx, sizeof motion_dx);
    memcpy(motion_filtered_dy, motion_dy, sizeof motion_dy);
    memcpy(motion_filtered_confident, motion_confident, sizeof motion_confident);
    for (int by = MOTION_WORLD_TOP / MOTION_BLOCK;
         by < (MOTION_WORLD_BOTTOM + MOTION_BLOCK - 1) / MOTION_BLOCK; ++by) {
        for (int bx = 0; bx < (width + MOTION_BLOCK - 1) / MOTION_BLOCK; ++bx) {
            int block = by * MOTION_BLOCKS_X + bx;
            int neighbors = 0;
            if (!motion_confident[block]) continue;
            for (int oy = -1; oy <= 1; ++oy) {
                for (int ox = -1; ox <= 1; ++ox) {
                    int nx = bx + ox, ny = by + oy;
                    int neighbor;
                    if (ox == 0 && oy == 0) continue;
                    if (nx < 0 || nx >= MOTION_BLOCKS_X || ny < 0 ||
                        ny >= MOTION_BLOCKS_Y) continue;
                    neighbor = ny * MOTION_BLOCKS_X + nx;
                    if (motion_confident[neighbor] &&
                        abs(motion_dx[neighbor] - motion_dx[block]) <= 2 &&
                        abs(motion_dy[neighbor] - motion_dy[block]) <= 2)
                        ++neighbors;
                }
            }
            if (neighbors == 0) motion_filtered_confident[block] = 0;
        }
    }
    memcpy(motion_dx, motion_filtered_dx, sizeof motion_dx);
    memcpy(motion_dy, motion_filtered_dy, sizeof motion_dy);
    memcpy(motion_confident, motion_filtered_confident, sizeof motion_confident);
}

static void motion_vector_at(int x, int y, double *dx, double *dy)
{
    double gx = ((double)x + 0.5) / MOTION_BLOCK - 0.5;
    double gy = ((double)y + 0.5) / MOTION_BLOCK - 0.5;
    int x0 = (int)floor(gx), y0 = (int)floor(gy);
    double fx = gx - x0, fy = gy - y0;
    double sum_x = 0.0, sum_y = 0.0, sum_w = 0.0;

    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            int bx = SDL_clamp(x0 + i, 0, MOTION_BLOCKS_X - 1);
            int by = SDL_clamp(y0 + j, 0, MOTION_BLOCKS_Y - 1);
            int block = by * MOTION_BLOCKS_X + bx;
            double weight = (i ? fx : 1.0 - fx) * (j ? fy : 1.0 - fy);
            if (!motion_confident[block]) continue;
            sum_x += motion_dx[block] * weight;
            sum_y += motion_dy[block] * weight;
            sum_w += weight;
        }
    }

    if (sum_w > 0.001) {
        *dx = sum_x / sum_w;
        *dy = sum_y / sum_w;
    } else {
        /* Do not force the world-camera displacement into ambiguous blocks.
         * Parallax backdrops move at a different rate; that fallback created
         * visible redraw seams where a confident local match met an uncertain
         * cloud/background block. Borrow nearby confident flow instead. */
        int bx0 = (int)floor(gx), by0 = (int)floor(gy);
        double nearby_x = 0.0, nearby_y = 0.0, nearby_w = 0.0;
        for (int by = by0 - 1; by <= by0 + 2; ++by) {
            for (int bx = bx0 - 1; bx <= bx0 + 2; ++bx) {
                int cx = SDL_clamp(bx, 0, MOTION_BLOCKS_X - 1);
                int cy = SDL_clamp(by, 0, MOTION_BLOCKS_Y - 1);
                int block = cy * MOTION_BLOCKS_X + cx;
                double ddx = cx - gx, ddy = cy - gy;
                double weight = 1.0 / (ddx * ddx + ddy * ddy + 0.5);
                if (!motion_confident[block]) continue;
                nearby_x += motion_dx[block] * weight;
                nearby_y += motion_dy[block] * weight;
                nearby_w += weight;
            }
        }
        if (nearby_w > 0.001) {
            *dx = nearby_x / nearby_w;
            *dy = nearby_y / nearby_w;
        } else {
            /* Leave fully ambiguous regions at the current frame position. */
            *dx = 0.0;
            *dy = 0.0;
        }
    }
}

void video_set_motion_interpolation(bool enabled)
{
    const char *preview = SDL_getenv("COSMO_PIXEL_CAMERA_PREVIEW");
    const char *steady = SDL_getenv("COSMO_STEADY_CAMERA_PREVIEW");
    const char *player = SDL_getenv("COSMO_PLAYER_PREVIEW");
    const char *star = SDL_getenv("COSMO_STAR_PREVIEW");
    if (motion_mutex) SDL_LockMutex(motion_mutex);
    smooth_gameplay_enabled = mods_feature_enabled(MOD_SMOOTH_GAMEPLAY);
    pixel_camera_preview = smooth_gameplay_enabled || (preview && SDL_strcmp(preview, "1") == 0);
    steady_camera_preview = smooth_gameplay_enabled ||
        (pixel_camera_preview && steady && SDL_strcmp(steady, "1") == 0);
    preview_player_enabled = smooth_gameplay_enabled ||
        (pixel_camera_preview && player && SDL_strcmp(player, "1") == 0);
    preview_current_player.visible = false;
    preview_current_player.tracked = false;
    preview_star_enabled = smooth_gameplay_enabled ||
        (preview_player_enabled && star && SDL_strcmp(star, "1") == 0);
    preview_stars.count = preview_previous_stars.count = 0;
    motion_interpolation_enabled = (enabled || pixel_camera_preview) && motion_mutex != NULL;
    motion_frame_count = 0;
    motion_vectors_valid = false;
    if (motion_mutex) SDL_UnlockMutex(motion_mutex);
}

void video_pause_motion_interpolation(bool paused)
{
    if (!motion_mutex) return;
    SDL_LockMutex(motion_mutex);
    bool was_paused = motion_interpolation_pause_depth != 0;
    if (paused) ++motion_interpolation_pause_depth;
    else if (motion_interpolation_pause_depth) --motion_interpolation_pause_depth;
    if (was_paused != (motion_interpolation_pause_depth != 0)) {
        /* Drop stale frames so an old gameplay frame cannot be mixed with a
         * hint popup, or used immediately after it closes. */
        motion_frame_count = 0;
        motion_vectors_valid = false;
    }
    SDL_UnlockMutex(motion_mutex);
}

uint8_t *video_preview_mask_row(unsigned page, unsigned y)
{
    if (!pixel_camera_preview || page >= 2 || y >= EGA_SCREEN_H) return NULL;
    return preview_page_mask[page][y];
}

void video_preview_begin_player(unsigned page)
{
    if (preview_player_enabled && page < 2)
        memset(&preview_player_pages[page], 0, sizeof preview_player_pages[page]);
    if (preview_star_enabled && page < 2) {
        preview_star_pages[page].count = 0;
        memset(preview_star_terrain[page], 0, sizeof preview_star_terrain[page]);
    }
}

static bool preview_sprite_available(unsigned page)
{
    return page < 2 && (!video_is_widescreen() || ega_wide_pages[page].visible) &&
        (smooth_gameplay_enabled ||
         (!mods_crt_shader_enabled() && !mods_crt_scanlines_enabled()));
}

bool video_preview_player_begin(unsigned page, int x, int y, bool in_front)
{
    if (!preview_player_enabled || !preview_sprite_available(page)) return false;
    preview_player_pages[page].visible = true;
    preview_player_pages[page].in_front = in_front;
    preview_player_pages[page].edge_padding = in_front;
    if (in_front)
        memset(preview_player_pages[page].occlusion, 0, sizeof preview_player_pages[page].occlusion);
    video_preview_player_track(page, x, y);
    return true;
}

void video_preview_player_track(unsigned page, int x, int y)
{
    if (!preview_player_enabled || !preview_sprite_available(page)) return;
    preview_player_pages[page].tracked = true;
    preview_player_pages[page].x = x;
    preview_player_pages[page].y = y;
}

static void preview_sprite_tile(preview_player *sprite, const uint8_t *pixels,
                                unsigned x, unsigned y, bool white)
{
    if (x >= PREVIEW_MASK_STRIDE || y >= EGA_SCREEN_H / 8) return;
    for (unsigned row = 0; row < 8; ++row) {
        const uint8_t *tile = pixels + row * 5;
        for (unsigned column = 0; column < 8; ++column) {
            unsigned bit = 0x80u >> column;
            if (tile[0] & bit) continue;
            unsigned color = white ? 15 : 0;
            if (!white) {
                for (unsigned plane = 0; plane < EGA_PLANES; ++plane)
                    if (tile[plane + 1] & bit) color |= 1u << plane;
            }
            sprite->pixels[y * 8 + row][x * 8 + column] = color + 1;
            int pixel_x = (int)(x * 8 + column);
            int pixel_y = (int)(y * 8 + row);
            if (sprite->right == 0 || pixel_x < sprite->left) sprite->left = pixel_x;
            if (pixel_x >= sprite->right) sprite->right = pixel_x + 1;
            if (sprite->bottom == 0 || pixel_y < sprite->top) sprite->top = pixel_y;
            if (pixel_y >= sprite->bottom) sprite->bottom = pixel_y + 1;
        }
    }
}

void video_preview_player_tile(unsigned page, const uint8_t *pixels,
                               unsigned x, unsigned y, bool white)
{
    if (page < 2) preview_sprite_tile(&preview_player_pages[page], pixels, x, y, white);
}

void video_preview_player_occlude(unsigned page, unsigned x, unsigned y, uint8_t bits)
{
    if (preview_player_enabled && page < 2 && x < PREVIEW_MASK_STRIDE && y < EGA_SCREEN_H)
        preview_player_pages[page].occlusion[y][x] |= bits;
}

bool video_preview_star_begin(unsigned page, uint32_t identity, int x, int y,
                              bool spin, bool in_front, bool expand, bool edge_padding)
{
    if (!preview_star_enabled || (!spin && !smooth_gameplay_enabled) ||
        !preview_sprite_available(page)) return false;
    preview_star_list *list = &preview_star_pages[page];
    if (!reserve_preview_stars(list, list->count + 1)) return false;
    preview_player *star = &list->sprites[list->count++];
    memset(star, 0, sizeof *star);
    if (!in_front)
        memcpy(star->occlusion, preview_star_terrain[page], sizeof star->occlusion);
    star->visible = true;
    star->spin = spin;
    star->expand = expand;
    star->edge_padding = edge_padding;
    star->identity = identity;
    star->x = x;
    star->y = y;
    return true;
}

void video_preview_effect_mark_slow_rise(unsigned page)
{
    if (page >= 2) return;
    preview_star_list *list = &preview_star_pages[page];
    if (list->count) list->sprites[list->count - 1].slow_rise = true;
}

void video_preview_star_tile(unsigned page, const uint8_t *pixels,
                             unsigned x, unsigned y, bool white)
{
    if (page >= 2) return;
    preview_star_list *list = &preview_star_pages[page];
    if (list->count)
        preview_sprite_tile(&list->sprites[list->count - 1], pixels, x, y, white);
}

void video_preview_star_occlude(unsigned page, unsigned x, unsigned y, uint8_t bits)
{
    if (!preview_star_enabled || page >= 2 || x >= PREVIEW_MASK_STRIDE || y >= EGA_SCREEN_H)
        return;
    preview_star_list *list = &preview_star_pages[page];
    if (!list->count) preview_star_terrain[page][y][x] |= bits;
    for (size_t i = 0; i < list->count; ++i)
        list->sprites[i].occlusion[y][x] |= bits;
}

static void capture_preview_backdrop(void)
{
    uint8_t lut[16][3];
    load_palette(lut);
    for (unsigned y = 0; y < PREVIEW_BACKDROP_H; ++y) {
        for (unsigned x = 0; x < PREVIEW_BACKDROP_W; ++x) {
            unsigned address = PREVIEW_BACKDROP_BASE +
                ((y / 8) * (PREVIEW_BACKDROP_W / 8) + x / 8) * 8 + y % 8;
            unsigned bit = 0x80u >> (x % 8), index = 0;
            for (unsigned plane = 0; plane < EGA_PLANES; ++plane) {
                if (ega.vram[plane][address] & bit) index |= 1u << plane;
            }
            memcpy(preview_backdrop[y][x], lut[index], 3);
        }
    }
}

static void advance_preview_clock(uint64_t now, unsigned frame_ticks)
{
    /* The game advances every 13 PIT ticks, or 10 in Turbo mode. */
    uint64_t period = SDL_GetPerformanceFrequency() * frame_ticks / 140u;
    if (!period || motion_frame_count == 0 || period != preview_frame_period ||
        now - motion_current_time > period * 2 ||
        now - motion_current_time < period / 2) {
        preview_frame_period = period;
        preview_frame_start = now;
        return;
    }

    /* Follow the simulation clock gradually without changing scroll speed
     * in response to each individual frame's arrival time. */
    uint64_t next = preview_frame_start + period;
    uint64_t correction = period / 8;
    if (now >= next) next += SDL_min(now - next, correction);
    else next -= SDL_min(next - now, correction);
    preview_frame_start = next;
}

void video_capture_motion_frame(uint16_t scroll_x, uint16_t scroll_y,
                                bool backdrop_x, bool backdrop_y, unsigned frame_ticks)
{
    uint64_t now;
    size_t bytes;

    if (!motion_mutex) return;
    bytes = (size_t)present_width * EGA_SCREEN_H * 3;
    SDL_LockMutex(motion_mutex);
    if (!motion_interpolation_enabled || motion_interpolation_pause_depth) {
        SDL_UnlockMutex(motion_mutex);
        return;
    }
    render_presentation(motion_capture, !smooth_gameplay_enabled);
    if (pixel_camera_preview) {
        unsigned page = ega.display_start / EGA_PAGE_SIZE;
        preview_layer_valid = preview_sprite_available(page);
        if (preview_layer_valid) {
            memcpy(preview_previous_mask, preview_current_mask, sizeof preview_previous_mask);
            memcpy(preview_current_mask, preview_page_mask[page], sizeof preview_current_mask);
            preview_backdrop_x = backdrop_x;
            preview_backdrop_y = backdrop_y;
            capture_preview_backdrop();
            if (preview_player_enabled) {
                preview_previous_player_tracked = motion_frame_count && preview_current_player.tracked &&
                    preview_current_player.in_front == preview_player_pages[page].in_front;
                preview_previous_player_x = preview_current_player.x;
                preview_previous_player_y = preview_current_player.y;
                preview_current_player = preview_player_pages[page];
                load_palette(preview_player_palette);
            }
            if (preview_star_enabled) {
                preview_star_list recycled = preview_previous_stars;
                preview_previous_stars = preview_stars;
                if (!motion_frame_count) preview_previous_stars.count = 0;
                preview_stars = preview_star_pages[page];
                for (size_t i = 0; i < preview_stars.count; ++i)
                    identify_blast_rings(&preview_stars.sprites[i]);
                preview_star_pages[page] = recycled;
                preview_star_pages[page].count = 0;
            }
        }
        if (!preview_layer_valid) {
            preview_current_player.visible = false;
            preview_current_player.tracked = false;
        }
        if (!preview_layer_valid) preview_stars.count = 0;
    }
    now = SDL_GetPerformanceCounter();
    if (steady_camera_preview) advance_preview_clock(now, frame_ticks);
    if (motion_frame_count != 0) {
        memcpy(motion_previous, motion_current, bytes);
        motion_previous_time = motion_current_time;
        motion_previous_camera_x = motion_current_camera_x;
        motion_previous_camera_y = motion_current_camera_y;
    } else {
        motion_previous_camera_x = scroll_x;
        motion_previous_camera_y = scroll_y;
    }
    memcpy(motion_current, motion_capture, bytes);
    motion_current_time = now;
    motion_current_camera_x = scroll_x;
    motion_current_camera_y = scroll_y;
    motion_camera_valid = abs((int)motion_current_camera_x - (int)motion_previous_camera_x) <= 2 &&
                          abs((int)motion_current_camera_y - (int)motion_previous_camera_y) <= 2;
    if (motion_frame_count < 2) ++motion_frame_count;
    if (motion_frame_count == 2 && !pixel_camera_preview)
        estimate_motion_vectors(motion_previous, motion_current, present_width);
    SDL_UnlockMutex(motion_mutex);
}

/* Move the foreground with the camera and sample the backdrop independently
 * at half speed on its scrolling axes. Keep screen-space pixels in place. */
static void render_pixel_camera(uint8_t *out, int delta_x, int delta_y, double alpha)
{
    const int left = 8;
    const int right = present_width - 8;
    const int shift_x = (int)lround((1.0 - alpha) * delta_x);
    const int shift_y = (int)lround((1.0 - alpha) * delta_y);
    if (shift_x == 0 && shift_y == 0) return;

    const int backdrop_x = preview_backdrop_x
        ? (int)motion_current_camera_x * 4 - (int)lround((1.0 - alpha) * delta_x / 2.0) : 0;
    const int backdrop_y = preview_backdrop_y
        ? (int)motion_current_camera_y * 4 - (int)lround((1.0 - alpha) * delta_y / 2.0) : 0;
    for (int y = MOTION_WORLD_TOP; y < MOTION_WORLD_BOTTOM; ++y) {
        size_t row = (size_t)y * present_width * 3;
        uint8_t *dest = out + row;
        const int by = (y - MOTION_WORLD_TOP + backdrop_y) % PREVIEW_BACKDROP_H;
        for (int x = left; x < right; ++x) {
            int source_x = x - shift_x;
            int source_y = y - shift_y;
            bool use_current = source_x >= left && source_x < right &&
                source_y >= MOTION_WORLD_TOP && source_y < MOTION_WORLD_BOTTOM;
            if (!use_current) {
                source_x += delta_x;
                source_y += delta_y;
                /* An opposite corner can be outside both views on diagonal moves. */
                if (source_x < left || source_x >= right ||
                    source_y < MOTION_WORLD_TOP || source_y >= MOTION_WORLD_BOTTOM) continue;
            }
            const uint8_t *mask = use_current
                ? preview_current_mask[source_y] : preview_previous_mask[source_y];
            const uint8_t *source = use_current ? motion_current : motion_previous;
            if (mask[source_x / 8] & (0x80u >> (source_x % 8))) {
                source += ((size_t)source_y * present_width + source_x) * 3;
            } else {
                int bx = (x - left + backdrop_x) % PREVIEW_BACKDROP_W;
                source = preview_backdrop[by][bx];
            }
            memcpy(dest + x * 3, source, 3);
        }
    }
}

static void render_blast_rings(uint8_t *out, const preview_player *sprite,
                               const preview_player *previous, double alpha,
                               int shift_x, int shift_y, int camera_x, int camera_y)
{
    int offset_x[BLAST_RING_LIMIT + 1] = {0}, offset_y[BLAST_RING_LIMIT + 1] = {0};
    bool matched[BLAST_RING_LIMIT] = {false};
    bool new_pulse = previous &&
        sprite->right - sprite->left + 8 < previous->right - previous->left;
    if (previous && previous->ring_count && !new_pulse) {
        int origin_dx = (sprite->x - previous->x) * 8;
        int origin_dy = (sprite->y - previous->y) * 8;
        for (unsigned i = 0; i < sprite->ring_count; ++i) {
            blast_ring ring = sprite->rings[i];
            int best = -1, distance = 17 * 17;
            for (unsigned j = 0; j < previous->ring_count; ++j) {
                if (matched[j]) continue;
                int dx = previous->rings[j].x + origin_dx - ring.x;
                int dy = previous->rings[j].y + origin_dy - ring.y;
                int squared = dx * dx + dy * dy;
                if (squared < distance) {
                    distance = squared;
                    best = (int)j;
                }
            }
            if (best < 0) continue;
            matched[best] = true;
            offset_x[i + 1] = (int)lround((1 - alpha) *
                (previous->rings[best].x + origin_dx - ring.x));
            offset_y[i + 1] = (int)lround((1 - alpha) *
                (previous->rings[best].y + origin_dy - ring.y));
        }
    }

    for (int sy = sprite->top; sy < sprite->bottom; ++sy) {
        for (int sx = sprite->left; sx < sprite->right; ++sx) {
            unsigned color = sprite->pixels[sy][sx];
            if (!color) continue;
            unsigned ring = sprite->ring_labels[sy - sprite->top][sx - sprite->left];
            int x = sx + shift_x + offset_x[ring];
            int y = sy + shift_y + offset_y[ring];
            if (x < 8 || x >= present_width - 8 ||
                y < MOTION_WORLD_TOP || y >= MOTION_WORLD_BOTTOM) continue;
            int mx = x - camera_x, my = y - camera_y;
            if (mx >= 8 && mx < present_width - 8 && my >= MOTION_WORLD_TOP &&
                my < MOTION_WORLD_BOTTOM &&
                (sprite->occlusion[my][mx / 8] & (0x80u >> (mx % 8)))) continue;
            memcpy(out + ((size_t)y * present_width + x) * 3,
                   preview_player_palette[color - 1], 3);
        }
    }
}

static void render_preview_sprite(uint8_t *out, const preview_player *sprite,
                                  bool previous_visible, int previous_x, int previous_y,
                                  const preview_player *previous,
                                  int dx, int dy, double alpha, double angle,
                                  int max_rise)
{
    if (!sprite->visible) return;
    int world_dx = dx + sprite->x - previous_x;
    int world_dy = dy + sprite->y - previous_y;
    int camera_x = (int)lround((1.0 - alpha) * dx * 8);
    int camera_y = (int)lround((1.0 - alpha) * dy * 8);
    int player_x = camera_x, player_y = camera_y;
    /* A cropped pose has missing rows. Do not drag those partial pixels
     * backward into the playfield as a sprite leaves through the top. */
    bool cropped_on_ascent = !sprite->edge_padding && world_dy < 0 &&
        sprite->top < MOTION_WORLD_TOP + 8;
    if (previous_visible && !cropped_on_ascent && abs(world_dx) <= 2 &&
        world_dy >= -max_rise && world_dy <= 2) {
        player_x -= (int)lround((1.0 - alpha) * world_dx * 8);
        double vertical_offset = -(1.0 - alpha) * world_dy * 8;
        /* The crusher climbs one tile every two ticks. Render half on each. */
        if (sprite->slow_rise && world_dy == -1)
            vertical_offset += alpha * 4;
        else if (previous && previous->slow_rise && world_dy == 0)
            vertical_offset = (1.0 - alpha) * 4;
        player_y += (int)lround(vertical_offset);
    }
    if (sprite->right <= sprite->left || sprite->bottom <= sprite->top) return;
    if (sprite->expand && sprite->ring_count) {
        render_blast_rings(out, sprite, previous_visible ? previous : NULL, alpha,
                           player_x, player_y, camera_x, camera_y);
        return;
    }
    double center_x = (sprite->left + sprite->right - 1) / 2.0;
    double center_y = (sprite->top + sprite->bottom - 1) / 2.0;
    double c = cos(angle), s = sin(angle);
    int left = sprite->left, right = sprite->right;
    int top = sprite->top, bottom = sprite->bottom;
    if (angle != 0) {
        double half_w = (right - left) / 2.0;
        double half_h = (bottom - top) / 2.0;
        double extent_x = fabs(c) * half_w + fabs(s) * half_h;
        double extent_y = fabs(s) * half_w + fabs(c) * half_h;
        left = (int)floor(center_x - extent_x);
        right = (int)ceil(center_x + extent_x) + 1;
        top = (int)floor(center_y - extent_y);
        bottom = (int)ceil(center_y + extent_y) + 1;
    }
    int first_x = SDL_max(8, left + player_x);
    int end_x = SDL_min(present_width - 8, right + player_x);
    int first_y = SDL_max(MOTION_WORLD_TOP, top + player_y);
    int end_y = SDL_min(MOTION_WORLD_BOTTOM, bottom + player_y);
    for (int y = first_y; y < end_y; ++y) {
        int mask_y = y - camera_y;
        for (int x = first_x; x < end_x; ++x) {
            int source_x = x - player_x;
            int source_y = y - player_y;
            if (angle != 0) {
                double rx = source_x - center_x, ry = source_y - center_y;
                source_x = (int)lround(center_x + c * rx + s * ry);
                source_y = (int)lround(center_y - s * rx + c * ry);
            }
            if (source_x < sprite->left || source_x >= sprite->right ||
                source_y < sprite->top || source_y >= sprite->bottom) continue;
            unsigned color = sprite->pixels[source_y][source_x];
            if (!color) continue;
            int mask_x = x - camera_x;
            if (mask_x >= 8 && mask_x < present_width - 8 &&
                mask_y >= MOTION_WORLD_TOP && mask_y < MOTION_WORLD_BOTTOM &&
                (sprite->occlusion[mask_y][mask_x / 8] & (0x80u >> (mask_x % 8))))
                continue;
            memcpy(out + ((size_t)y * present_width + x) * 3,
                   preview_player_palette[color - 1], 3);
        }
    }
}

static void render_preview_sprites(uint8_t *out, int dx, int dy, double alpha)
{
    /* The first jump tick can combine a one-tile uphill step and a two-tile lift. */
    render_preview_sprite(out, &preview_current_player, preview_previous_player_tracked,
                          preview_previous_player_x, preview_previous_player_y, NULL,
                          dx, dy, alpha, 0, 3);
    /* Four original poses cover one 72-degree interval of a five-point star.
     * Approach each original pose on the game clock. */
    const double pose_step = 0.3141592653589793; /* 18 degrees */
    for (size_t i = 0; i < preview_stars.count; ++i) {
        const preview_player *star = &preview_stars.sprites[i];
        const preview_player *previous = NULL;
        for (size_t j = 0; j < preview_previous_stars.count; ++j) {
            if (preview_previous_stars.sprites[j].identity == star->identity) {
                previous = &preview_previous_stars.sprites[j];
                break;
            }
        }
        /* Hold one midpoint, then show the original pose for half the tick. */
        double angle = star->spin && previous && alpha < 0.5 ? pose_step * 0.5 : 0;
        render_preview_sprite(out, star, previous != NULL,
                              previous ? previous->x : star->x,
                              previous ? previous->y : star->y, previous, dx, dy, alpha, angle, 2);
    }
}

static bool render_motion_frame(uint8_t *out)
{
    uint64_t now, interval, target;
    double alpha;
    size_t bytes;
    bool camera_moved;

    if (!motion_mutex) return false;
    SDL_LockMutex(motion_mutex);
    if (!motion_interpolation_enabled || motion_interpolation_pause_depth) {
        SDL_UnlockMutex(motion_mutex);
        return false;
    }
    camera_moved = motion_current_camera_x != motion_previous_camera_x ||
                   motion_current_camera_y != motion_previous_camera_y;
    bool sprite_visible = pixel_camera_preview &&
        (preview_current_player.visible || preview_stars.count != 0);
    if (sprite_visible && (motion_frame_count == 1 || !motion_camera_valid)) {
        memcpy(out, motion_current, (size_t)present_width * EGA_SCREEN_H * 3);
        render_preview_sprites(out, 0, 0, 1.0);
        SDL_UnlockMutex(motion_mutex);
        return true;
    }
    if (motion_frame_count < 2 || (!motion_vectors_valid && !camera_moved && !sprite_visible) ||
        !motion_camera_valid ||
        motion_current_time <= motion_previous_time) {
        SDL_UnlockMutex(motion_mutex);
        return false;
    }

    bytes = (size_t)present_width * EGA_SCREEN_H * 3;
    memcpy(out, motion_current, bytes);
    now = SDL_GetPerformanceCounter();
    interval = motion_current_time - motion_previous_time;
    target = now > interval ? now - interval : 0;
    if (target <= motion_previous_time) alpha = 0.0;
    else if (target >= motion_current_time) alpha = 1.0;
    else alpha = (double)(target - motion_previous_time) / (double)interval;

    if (pixel_camera_preview) {
        int dx = (int)motion_current_camera_x - (int)motion_previous_camera_x;
        int dy = (int)motion_current_camera_y - (int)motion_previous_camera_y;
        /* The shared camera validity check already bounds each axis to two
         * tiles, including the faster camera steps during falls. */
        if (!preview_layer_valid) {
            if (sprite_visible) render_preview_sprites(out, 0, 0, 1.0);
            SDL_UnlockMutex(motion_mutex);
            return sprite_visible;
        }
        if (steady_camera_preview && preview_frame_period != 0) {
            uint64_t elapsed = now > preview_frame_start ? now - preview_frame_start : 0;
            alpha = (double)SDL_min(elapsed, preview_frame_period) / preview_frame_period;
        }
        render_pixel_camera(out, dx * 8, dy * 8, alpha);
        render_preview_sprites(out, dx, dy, alpha);
        SDL_UnlockMutex(motion_mutex);
        return true;
    }

    for (int y = MOTION_WORLD_TOP; y < MOTION_WORLD_BOTTOM && y < EGA_SCREEN_H; ++y) {
        for (int x = 0; x < present_width; ++x) {
            double dx, dy;
            int sx, sy;
            uint8_t *dest;
            const uint8_t *source;
            motion_vector_at(x, y, &dx, &dy);
            sx = x + (int)lround((1.0 - alpha) * dx);
            sy = y + (int)lround((1.0 - alpha) * dy);
            if (sx < 0 || sx >= present_width || sy < MOTION_WORLD_TOP ||
                sy >= MOTION_WORLD_BOTTOM || sy >= EGA_SCREEN_H) continue;
            dest = out + ((size_t)y * (size_t)present_width + (size_t)x) * 3;
            source = motion_current + ((size_t)sy * (size_t)present_width +
                                       (size_t)sx) * 3;
            memcpy(dest, source, 3);
        }
    }
    SDL_UnlockMutex(motion_mutex);
    return true;
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

    render_presentation(rgb, true);

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

#ifndef COSMO_VITA_APP
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
#endif

bool video_init(const char *title, int scale, bool widescreen)
{
    touch_controls = false;
    touch_context = TOUCH_HIDDEN;
    SDL_SetAtomicInt(&touch_hud_enabled, false);
    touch_pressed = 0;
    SDL_SetAtomicInt(&touch_bomb_ready, 0);
    int width = widescreen ? WIDE_PRESENT_W : EGA_SCREEN_W;
    int height = widescreen ? WIDE_WINDOW_H : CORRECTED_EGA_H;

    if (scale < 1) scale = 3;

    bool reuse_window = window != NULL;
    if (!reuse_window && !SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }

#ifdef COSMO_VITA_APP
    scale = SDL_min(scale, 2);
#else
    scale = fitting_scale(scale, width, height);
#endif

    int w = width * scale;
    int h = height * scale;

    if (!reuse_window &&
        !SDL_CreateWindowAndRenderer(title, w, h, SDL_WINDOW_RESIZABLE,
                                     &window, &renderer)) {
        fprintf(stderr, "SDL_CreateWindowAndRenderer: %s\n", SDL_GetError());
        return false;
    }

    {
        SDL_DisplayID display = SDL_GetDisplayForWindow(window);
        const SDL_DisplayMode *mode = display ? SDL_GetCurrentDisplayMode(display) : NULL;
        if (mode && mode->refresh_rate >= 30.0f && mode->refresh_rate <= 360.0f)
            display_refresh_hz = mode->refresh_rate;
        renderer_vsync = SDL_SetRenderVSync(renderer, 1);
        if (!renderer_vsync) {
            SDL_Log("VSync unavailable (%s); pacing at %.2f Hz",
                    SDL_GetError(), display_refresh_hz);
        } else {
            SDL_Log("VSync enabled at display refresh %.2f Hz", display_refresh_hz);
        }
    }

    /* Letterbox to the corrected aspect, so resizing never distorts. */
    SDL_SetRenderLogicalPresentation(renderer, w, h,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);

    SDL_Texture *new_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24,
                                               SDL_TEXTUREACCESS_STREAMING,
                                               width, EGA_SCREEN_H);
    if (!new_texture) {
        fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
        if (!reuse_window) video_shutdown();
        return false;
    }
    if (!motion_mutex) motion_mutex = SDL_CreateMutex();
    if (!motion_mutex) {
        fprintf(stderr, "SDL_CreateMutex: %s\n", SDL_GetError());
        SDL_DestroyTexture(new_texture);
        if (!reuse_window) video_shutdown();
        return false;
    }
    if (texture) SDL_DestroyTexture(texture);
    texture = new_texture;
    motion_interpolation_enabled = false;
    motion_interpolation_pause_depth = 0;
    motion_vectors_valid = false;
    motion_frame_count = 0;
    present_width = width;
    if (widescreen) {
        wide_destination = (SDL_FRect){
            0, WIDE_CONTENT_Y * scale, w, CORRECTED_EGA_H * scale
        };
    }

    /* Chunky pixels, not a blurry upscale. */
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);

    if (reuse_window) {
        SDL_SetWindowTitle(window, title);
    } else {
        SDL_RestoreWindow(window);
        SDL_RaiseWindow(window);
    }

    return true;
}

enum { FRAME_CAPTURE_COUNT = 120 };
static uint8_t *frame_capture;
static uint64_t frame_capture_times[FRAME_CAPTURE_COUNT];
static unsigned frame_capture_count;
static int frame_capture_width;

void video_start_frame_capture(void)
{
    if (frame_capture) return;
    frame_capture_width = present_width;
    frame_capture_count = 0;
    frame_capture = malloc((size_t)FRAME_CAPTURE_COUNT * present_width * EGA_SCREEN_H * 3);
    if (!frame_capture) SDL_Log("Could not allocate frame capture buffer");
    else SDL_Log("Capturing the next %d presented frames", FRAME_CAPTURE_COUNT);
}

static void save_frame_capture(void)
{
    if (!frame_capture) return;
    const char *root = SDL_getenv("COSMO_FRAME_CAPTURE_DIR");
    if (!root || !*root) root = "Frame Captures";
    char *folder = NULL;
    SDL_Time stamp;
    if (frame_capture_count && SDL_CreateDirectory(root) && SDL_GetCurrentTime(&stamp) &&
        SDL_asprintf(&folder, "%s/burst-%lld", root, (long long)stamp) >= 0 &&
        SDL_CreateDirectory(folder)) {
        size_t frame_bytes = (size_t)frame_capture_width * EGA_SCREEN_H * 3;
        unsigned saved = 0;
        for (unsigned i = 0; i < frame_capture_count; ++i) {
            char *path = NULL;
            uint64_t elapsed_us = (frame_capture_times[i] - frame_capture_times[0]) / 1000;
            if (SDL_asprintf(&path, "%s/frame-%03u-%09lluus.png", folder, i + 1,
                             (unsigned long long)elapsed_us) >= 0 &&
                png_write_rgb(path, frame_capture + i * frame_bytes,
                              frame_capture_width, EGA_SCREEN_H)) ++saved;
            SDL_free(path);
        }
        SDL_Log("Saved %u of %u frames in %s", saved, frame_capture_count, folder);
    } else if (frame_capture_count) {
        SDL_Log("Could not create the frame capture directory");
    }
    SDL_free(folder);
    free(frame_capture);
    frame_capture = NULL;
    frame_capture_count = 0;
}

void video_present(void)
{
    uint8_t *pixels;
    int pitch;
    int row_bytes = present_width * 3;
    bool interpolated = render_motion_frame(motion_output);
    if (interpolated && smooth_gameplay_enabled)
        apply_display_filters(motion_output, present_width);

    if (!SDL_LockTexture(texture, NULL, (void **)&pixels, &pitch)) return;

    if (pitch == row_bytes) {
        if (interpolated) memcpy(pixels, motion_output, (size_t)row_bytes * EGA_SCREEN_H);
        else render_presentation(pixels, true);
    } else {
        if (!interpolated) render_presentation(motion_output, true);
        for (int y = 0; y < EGA_SCREEN_H; y++) {
            memcpy(pixels + (size_t)y * (size_t)pitch,
                   motion_output + (size_t)y * (size_t)row_bytes,
                   (size_t)row_bytes);
        }
    }

    bool touch_hud = touch_controls && video_is_widescreen();

    if (frame_capture && present_width != frame_capture_width) save_frame_capture();
    if (frame_capture) {
        size_t row_size = (size_t)row_bytes;
        uint8_t *dest = frame_capture + frame_capture_count * row_size * EGA_SCREEN_H;
        for (int y = 0; y < EGA_SCREEN_H; ++y) {
            memcpy(dest + (size_t)y * row_size, pixels + (size_t)y * pitch, row_size);
        }
    }
    SDL_UnlockTexture(texture);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_FRect touch_playfield = {TOUCH_GUTTER, 0, EGA_SCREEN_W, CORRECTED_EGA_H};
    SDL_FRect touch_wide = {0, WIDE_CONTENT_Y, WIDE_PRESENT_W, CORRECTED_EGA_H};
    SDL_RenderTexture(renderer, texture, NULL,
                      touch_hud ? &touch_wide : touch_controls ? &touch_playfield :
                      video_is_widescreen() ? &wide_destination : NULL);
    if (touch_controls && touch_context != TOUCH_HIDDEN) render_touch_controls(touch_hud);
    if (SDL_RenderPresent(renderer) && frame_capture) {
        frame_capture_times[frame_capture_count++] = SDL_GetTicksNS();
        if (frame_capture_count == FRAME_CAPTURE_COUNT) save_frame_capture();
    }

    /* Keep refresh cadence on drivers that cannot provide SDL VSync. */
    if (!renderer_vsync) {
        uint64_t freq = SDL_GetPerformanceFrequency();
        uint64_t period = (uint64_t)((double)freq / display_refresh_hz);
        uint64_t now = SDL_GetPerformanceCounter();
        if (period == 0) period = 1;
        if (next_manual_present <= now || next_manual_present - now > period)
            next_manual_present = now + period;
        else {
            uint64_t remaining = next_manual_present - now;
            SDL_DelayNS((uint64_t)((double)remaining * 1000000000.0 / (double)freq));
            next_manual_present += period;
        }
    }
}

bool video_is_fullscreen(void)
{
    return window && (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN);
}

void video_set_fullscreen(bool fullscreen)
{
    if (fullscreen) {
        SDL_DisplayID display = SDL_GetDisplayForWindow(window);
        int window_w = 0, window_h = 0, mode_count = 0;
        SDL_DisplayMode **modes;
        SDL_DisplayMode *best = NULL;
        float best_difference = 1000.0f;

        SDL_GetWindowSizeInPixels(window, &window_w, &window_h);
        modes = display ? SDL_GetFullscreenDisplayModes(display, &mode_count) : NULL;
        for (int i = 0; modes && i < mode_count; ++i) {
            SDL_DisplayMode *mode = modes[i];
            float difference;

            if (!mode || mode->w < window_w || mode->h < window_h ||
                mode->refresh_rate < 100.0f || mode->refresh_rate > 360.0f)
                continue;

            difference = SDL_fabsf(mode->refresh_rate - 120.0f);
            if (!best || difference < best_difference ||
                (difference == best_difference && mode->w * mode->h > best->w * best->h)) {
                best = mode;
                best_difference = difference;
            }
        }

        if (best && SDL_SetWindowFullscreenMode(window, best)) {
            SDL_Log("Requesting fullscreen mode %dx%d at %.2f Hz",
                    best->w, best->h, best->refresh_rate);
        } else {
            /* Borderless fullscreen follows the user's existing desktop mode. */
            if (!SDL_SetWindowFullscreenMode(window, NULL))
                SDL_Log("Could not select fullscreen mode: %s", SDL_GetError());
            SDL_Log("No compatible 100+ Hz fullscreen mode; using desktop mode");
        }
        if (modes) SDL_free(modes);
    } else if (!SDL_SetWindowFullscreenMode(window, NULL)) {
        SDL_Log("Could not restore desktop fullscreen mode: %s", SDL_GetError());
    }

    if (!SDL_SetWindowFullscreen(window, fullscreen) || !SDL_SyncWindow(window)) {
        SDL_Log("Could not change fullscreen mode: %s", SDL_GetError());
        return;
    }

    {
        SDL_DisplayID display = SDL_GetDisplayForWindow(window);
        const SDL_DisplayMode *mode = display ? SDL_GetCurrentDisplayMode(display) : NULL;
        if (mode && mode->refresh_rate >= 30.0f && mode->refresh_rate <= 360.0f)
            display_refresh_hz = mode->refresh_rate;
        if (!SDL_SetRenderVSync(renderer, 1)) {
            renderer_vsync = false;
            SDL_Log("VSync unavailable after fullscreen switch (%s)", SDL_GetError());
        } else {
            renderer_vsync = true;
            SDL_Log("Fullscreen VSync active at %.2f Hz", display_refresh_hz);
        }
    }
}

bool video_poll_event(SDL_Event *event)
{
    while (SDL_PollEvent(event)) {
        /* SDL3 reports the title-bar X as a per-window close request. Turn it
         * into the app-wide quit event consumed by both launcher and game. */
        if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            event->type = SDL_EVENT_QUIT;
        }
        if ((event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) &&
            event->key.key == SDLK_AC_BACK) {
            event->key.key = SDLK_ESCAPE;
            event->key.scancode = SDL_SCANCODE_ESCAPE;
        }
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
    save_frame_capture();
    SDL_free(preview_stars.sprites);
    SDL_free(preview_previous_stars.sprites);
    preview_stars = preview_previous_stars = (preview_star_list){0};
    for (unsigned page = 0; page < 2; ++page) {
        SDL_free(preview_star_pages[page].sprites);
        preview_star_pages[page] = (preview_star_list){0};
    }
    if (launcher_touch_texture) SDL_DestroyTexture(launcher_touch_texture);
    launcher_touch_texture = NULL;
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    texture = NULL;
    renderer = NULL;
    window = NULL;
    if (motion_mutex) SDL_DestroyMutex(motion_mutex);
    motion_mutex = NULL;
    motion_interpolation_enabled = false;
    motion_vectors_valid = false;
    motion_frame_count = 0;
    present_width = EGA_SCREEN_W;
    renderer_vsync = false;
    display_refresh_hz = 60.0;
    next_manual_present = 0;
    SDL_Quit();
}
