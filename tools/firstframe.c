/*
 * firstframe.c -- Validation harness for the video layer.
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
#include "cosmo/video.h"

#define FULLSCREEN_IMAGE_SIZE 32000
#define GROUP_HEADER_SIZE     960
#define GROUP_ENTRY_SIZE      20
#define GROUP_NAME_COMPARE    11

static uint32_t read_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * Locate an entry in a group file. Mirrors GroupEntryFp() (game2.c:1472):
 * a 960-byte header of 20-byte records, each holding a 12-byte name followed
 * by a 32-bit offset and a 32-bit length.
 */
static long group_find(const char *path, const char *entry_name, uint32_t *length)
{
    unsigned char header[GROUP_HEADER_SIZE];
    FILE *fp = fopen(path, "rb");

    if (!fp) return -1;

    if (fread(header, 1, sizeof header, fp) != sizeof header) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    for (size_t i = 0; i < sizeof header; i += GROUP_ENTRY_SIZE) {
        if (header[i] == '\0') break;  /* no more entries */

        if (strncmp((const char *)header + i, entry_name,
                    GROUP_NAME_COMPARE) == 0) {
            *length = read_le32(header + i + 16);
            return (long)read_le32(header + i + 12);
        }
    }
    return -1;
}

static bool load_entry(const char *datadir, const char *entry_name,
                       unsigned char *dest, size_t want)
{
    /* The game tries the STN first, then the VOL. */
    static const char *groups[] = {"COSMO1.STN", "COSMO1.VOL"};

    for (size_t g = 0; g < sizeof groups / sizeof groups[0]; g++) {
        char path[1024];
        uint32_t length = 0;
        long offset;
        FILE *fp;
        size_t got;

        snprintf(path, sizeof path, "%s/%s", datadir, groups[g]);
        offset = group_find(path, entry_name, &length);
        if (offset < 0) continue;

        fp = fopen(path, "rb");
        if (!fp) continue;
        if (fseek(fp, offset, SEEK_SET) != 0) { fclose(fp); continue; }

        got = fread(dest, 1, want, fp);
        fclose(fp);

        printf("  %s found in %s (offset %ld, length %u, read %zu)\n",
               entry_name, groups[g], offset, length, got);
        return got == want;
    }

    fprintf(stderr, "  %s not found in any group file under %s/\n",
            entry_name, datadir);
    return false;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [datadir] [entry] [output.png]\n"
        "\n"
        "  datadir     directory holding COSMO1.STN and COSMO1.VOL"
        " (default: gamedata)\n"
        "  entry       group entry to render (default: TITLE1.MNI)\n"
        "  output.png  write a screenshot instead of opening a window\n",
        argv0);
}

int main(int argc, char *argv[])
{
    const char *datadir  = (argc > 1) ? argv[1] : "gamedata";
    const char *entry    = (argc > 2) ? argv[2] : "TITLE1.MNI";
    const char *png_path = (argc > 3) ? argv[3] : NULL;
    unsigned char image[FULLSCREEN_IMAGE_SIZE];

    if (argc > 1 && (strcmp(argv[1], "-h") == 0 ||
                     strcmp(argv[1], "--help") == 0)) {
        usage(argv[0]);
        return 0;
    }

    printf("Loading %s from %s/\n", entry, datadir);
    if (!load_entry(datadir, entry, image, sizeof image)) return 1;

    ega_init();
    ega_set_video_mode(0x0D);

    /* EGA_MODE_DEFAULT() and EGA_BIT_MASK_DEFAULT(), from lowlevel.h */
    ega_out_word(EGA_PORT_GC_INDEX, (0x00 << 8) | GC_MODE);
    ega_out_word(EGA_PORT_GC_INDEX, (0xFF << 8) | GC_BIT_MASK);

    /* DrawFullscreenImage()'s loop: one plane at a time, 8000 bytes each. */
    {
        unsigned mask = 0x0100;
        for (unsigned srcbase = 0; srcbase < FULLSCREEN_IMAGE_SIZE;
             srcbase += EGA_PAGE_BYTES) {
            ega_out_word(EGA_PORT_SEQ_INDEX, (uint16_t)(SEQ_MAP_MASK | mask));
            for (unsigned i = 0; i < EGA_PAGE_BYTES; i++) {
                ega_write(i, image[i + srcbase]);
            }
            mask <<= 1;
        }
    }

    ega_select_active_page(0);

    if (png_path) {
        if (!video_write_png(png_path)) {
            fprintf(stderr, "could not write %s\n", png_path);
            return 1;
        }
        printf("Wrote %s (%dx%d)\n", png_path, EGA_SCREEN_W, EGA_SCREEN_H);
        return 0;
    }

    if (!video_init("Cosmo -- first frame", 3)) return 1;
    while (video_pump_events()) {
        video_present();
        video_delay(16);
    }
    video_shutdown();
    return 0;
}
