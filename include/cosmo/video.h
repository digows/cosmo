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

/* Window management. `scale` is the horizontal factor; height is corrected
 * to 4:3 because mode 0Dh pixels are taller than they are wide. */
bool video_init(const char *title, int scale);
void video_present(void);
void video_shutdown(void);

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

#endif /* COSMO_VIDEO_H */
