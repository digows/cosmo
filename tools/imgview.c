/*
 * imgview.c -- Validation harness for the video layer.
 *
 * This is not part of the port. It exists to prove, end to end, that:
 *   1. we can read the game's STN/VOL group files;
 *   2. the emulated EGA applies map mask and write mode like the real hardware;
 *   3. de-interleaving the four planes and applying the palette yields the
 *      image the artists drew in 1992.
 *
 * The write loop below is a faithful copy of DrawFullscreenImage()
 * (game1.c:558) from Cosmore: same plane order, same port values. The only
 * difference is that stores go through ega_write() instead of a far pointer
 * into segment 0xA000.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cosmo/ega.h"
#include "cosmo/group.h"
#include "cosmo/video.h"

#define FULLSCREEN_IMAGE_SIZE 32000

typedef uint16_t word;

/* The game's own fullscreenImageNames[] table (game1.c:107), episode 1. */
static const char *const fullscreen_images[] = {
    "PRETITLE.MNI", "TITLE1.MNI", "CREDIT.MNI",
    "BONUS.MNI", "END1.MNI", "ONEMOMNT.MNI"
};
#define IMAGE_COUNT ((int)(sizeof fullscreen_images / sizeof fullscreen_images[0]))

static bool load_entry(const char *datadir, const char *entry_name,
                       unsigned char *dest, size_t want, bool verbose)
{
    size_t got = group_read(datadir, 1, entry_name, dest, want);

    if (got != want) {
        fprintf(stderr, "  %s not found in the group files under %s/\n",
                entry_name, datadir);
        return false;
    }

    if (verbose) printf("  %s: %zu bytes\n", entry_name, got);
    return true;
}

/* DrawFullscreenImage()'s loop: one plane at a time, 8000 bytes each. */
static void upload_fullscreen_image(const unsigned char *image)
{
    unsigned mask = 0x0100;

    ega_set_video_mode(0x0D);

    /* EGA_MODE_DEFAULT() and EGA_BIT_MASK_DEFAULT(), from lowlevel.h */
    ega_out_word(EGA_PORT_GC_INDEX, (0x00 << 8) | GC_MODE);
    ega_out_word(EGA_PORT_GC_INDEX, (0xFF << 8) | GC_BIT_MASK);

    for (unsigned srcbase = 0; srcbase < FULLSCREEN_IMAGE_SIZE;
         srcbase += EGA_PAGE_BYTES) {
        ega_out_word(EGA_PORT_SEQ_INDEX, (uint16_t)(SEQ_MAP_MASK | mask));
        for (unsigned i = 0; i < EGA_PAGE_BYTES; i++) {
            ega_write(i, image[i + srcbase]);
        }
        mask <<= 1;
    }

    ega_select_active_page(0);

    /*
     * Install the palette the game itself ends up with. DrawFullscreenImage()
     * finishes by calling FadeIn(), which writes register N with value N, and
     * N + 8 from register 8 up. Without this the BIOS default palette is used,
     * which differs in a couple of entries from what the artwork expects.
     */
    {
        word skip = 0;
        for (word reg = 0; reg < 16; reg++) {
            if (reg == 8) skip = 8;
            ega_set_palette_register((uint8_t)reg, (uint8_t)(reg + skip));
        }
    }
}

static bool show(const char *datadir, const char *entry, bool verbose)
{
    unsigned char image[FULLSCREEN_IMAGE_SIZE];
    char title[256];

    if (!load_entry(datadir, entry, image, sizeof image, verbose)) return false;

    upload_fullscreen_image(image);
    snprintf(title, sizeof title, "Cosmo -- %s", entry);
    video_set_title(title);
    return true;
}

static int render_to_png(const char *datadir, const char *entry,
                         const char *png_path, int scale)
{
    unsigned char image[FULLSCREEN_IMAGE_SIZE];

    if (!load_entry(datadir, entry, image, sizeof image, true)) return 1;

    ega_init();
    upload_fullscreen_image(image);

    if (!video_write_png(png_path, scale)) {
        fprintf(stderr, "could not write %s\n", png_path);
        return 1;
    }

    printf("Wrote %s (%dx%d)\n", png_path,
           EGA_SCREEN_W * scale, EGA_SCREEN_H * scale);
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [datadir] [entry] [output.png] [scale]\n"
        "\n"
        "  datadir     directory holding COSMO1.STN and COSMO1.VOL"
        " (default: gamedata)\n"
        "  entry       group entry to show; omit to browse all of them\n"
        "  output.png  write a screenshot instead of opening a window\n"
        "  scale       integer magnification for the screenshot (default 1)\n"
        "\n"
        "In the window: arrows or space to change image, S to save a\n"
        "screenshot, Q or Escape to quit.\n",
        argv0);
}

int main(int argc, char *argv[])
{
    const char *datadir  = (argc > 1) ? argv[1] : "gamedata";
    const char *entry    = (argc > 2) ? argv[2] : NULL;
    const char *png_path = (argc > 3) ? argv[3] : NULL;
    int scale = (argc > 4) ? atoi(argv[4]) : 1;
    int index = 0;

    if (argc > 1 && (strcmp(argv[1], "-h") == 0 ||
                     strcmp(argv[1], "--help") == 0)) {
        usage(argv[0]);
        return 0;
    }

    if (scale < 1) scale = 1;

    if (png_path) {
        return render_to_png(datadir, entry ? entry : fullscreen_images[0],
                             png_path, scale);
    }

    /* Browsing mode. A named entry just picks the starting point. */
    if (entry) {
        for (int i = 0; i < IMAGE_COUNT; i++) {
            if (strcmp(entry, fullscreen_images[i]) == 0) { index = i; break; }
        }
    }

    ega_init();

    if (!video_init("Cosmo", 3)) return 1;

    printf("Browsing %d fullscreen images from %s/\n", IMAGE_COUNT, datadir);
    printf("Arrows or space to change image, S to save a screenshot, Q to quit.\n");

    if (!show(datadir, entry ? entry : fullscreen_images[index], true)) {
        video_shutdown();
        return 1;
    }

    for (;;) {
        video_key key = video_poll_key();

        if (key == VIDEO_KEY_QUIT) break;

        if (key == VIDEO_KEY_NEXT || key == VIDEO_KEY_PREV) {
            index += (key == VIDEO_KEY_NEXT) ? 1 : IMAGE_COUNT - 1;
            index %= IMAGE_COUNT;
            show(datadir, fullscreen_images[index], false);
        } else if (key == VIDEO_KEY_SCREENSHOT) {
            char path[64];
            snprintf(path, sizeof path, "cosmo-%d.png", index);
            if (video_write_png(path, 2)) printf("Wrote %s\n", path);
        }

        video_present();
        video_delay(16);
    }

    video_shutdown();
    return 0;
}
