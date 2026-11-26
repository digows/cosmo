/*
 * video.h -- Presentation: planar EGA memory to screen.
 */

#ifndef COSMO_VIDEO_H
#define COSMO_VIDEO_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Convert the currently displayed EGA page to 8-bit-per-channel RGB.
 * `out` must have room for EGA_SCREEN_W * EGA_SCREEN_H * 3 bytes.
 */
void video_render_rgb(uint8_t *out);

/*
 * Write the current screen as a PNG, enlarged by an integer `scale` using
 * nearest-neighbour sampling so the pixels stay square-edged. Returns false on
 * I/O failure.
 */
bool video_write_png(const char *path, int scale);

/* Window management. `scale` is the horizontal factor. The output canvas
 * is 4:3 or 16:9, with the original pixel proportions in either mode. */
bool video_init(const char *title, int scale, bool widescreen);
bool video_is_widescreen(void);
void video_present(void);
enum { TOUCH_LEFT, TOUCH_RIGHT, TOUCH_UP, TOUCH_DOWN, TOUCH_JUMP, TOUCH_BOMB,
       TOUCH_PAUSE, TOUCH_BUTTON_COUNT };
union SDL_Event;
void video_set_touch_controls(bool enabled);
void video_load_launcher_touch_texture(void);
enum { TOUCH_HIDDEN, TOUCH_MENU, TOUCH_PLAY };
void video_set_touch_context(unsigned context);
void video_set_touch_pressed(unsigned buttons);
void video_set_touch_bomb_sprite(const uint8_t *tiles, unsigned columns, unsigned rows);
int video_touch_button(union SDL_Event *event);
void video_start_frame_capture(void);
void video_set_motion_interpolation(bool enabled);
void video_pause_motion_interpolation(bool paused);
void video_capture_motion_frame(uint16_t scroll_x, uint16_t scroll_y,
                                bool backdrop_x, bool backdrop_y, unsigned frame_ticks);
uint8_t *video_preview_mask_row(unsigned page, unsigned y);
void video_preview_begin_player(unsigned page);
bool video_preview_player_begin(unsigned page, int x, int y, bool in_front);
void video_preview_player_track(unsigned page, int x, int y);
void video_preview_player_tile(unsigned page, const uint8_t *pixels,
                               unsigned x, unsigned y, bool white);
void video_preview_player_occlude(unsigned page, unsigned x, unsigned y, uint8_t bits);
bool video_preview_star_begin(unsigned page, uint32_t identity, int x, int y,
                              bool spin, bool in_front, bool expand, bool edge_padding);
void video_preview_effect_mark_slow_rise(unsigned page);
void video_preview_star_tile(unsigned page, const uint8_t *pixels,
                             unsigned x, unsigned y, bool white);
void video_preview_star_occlude(unsigned page, unsigned x, unsigned y, uint8_t bits);
void video_shutdown(void);
bool video_is_fullscreen(void);
void video_set_fullscreen(bool fullscreen);

/* Handle display shortcuts before passing events to the caller. */
bool video_poll_event(union SDL_Event *event);

/*
 * Minimal input for the standalone tools. This is not the game's keyboard
 * path -- Cosmo installs its own int 9 handler and reads scancodes from port
 * 0x60, which the interrupt layer will provide. This exists so the harnesses
 * can be driven at all.
 */
typedef enum {
    VIDEO_KEY_NONE = 0,
    VIDEO_KEY_QUIT,
    VIDEO_KEY_NEXT,
    VIDEO_KEY_PREV,
    VIDEO_KEY_SCREENSHOT
} video_key;

video_key video_poll_key(void);

/* Update the window caption. */
void video_set_title(const char *title);

/* Sleep, used by the standalone tools. Wraps the SDL implementation so
 * callers do not need to include SDL headers. */
void video_delay(uint32_t ms);

/* The window, for anything that has to be parented to it. NULL before init. */
struct SDL_Window *video_get_window(void);

#endif /* COSMO_VIDEO_H */
