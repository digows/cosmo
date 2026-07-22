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

/* Returns false once the user closes the window or presses Escape. */
bool video_pump_events(void);

/* Sleep, used by the standalone tools. Wraps the SDL implementation so
 * callers do not need to include SDL headers. */
void video_delay(uint32_t ms);

#endif /* COSMO_VIDEO_H */
