/*
 * group.c -- Reading the game's STN and VOL group files.
 */

#include <stdio.h>
#include <string.h>

#include "cosmo/group.h"

#define GROUP_NAME_COMPARE 11

static uint32_t read_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static long find_entry(const char *path, const char *entry_name,
                       uint32_t *length)
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

size_t group_read(const char *datadir, int episode, const char *entry_name,
                  void *dest, size_t max)
{
    static const char *const extensions[] = {"STN", "VOL"};

    for (size_t g = 0; g < sizeof extensions / sizeof extensions[0]; g++) {
        char path[1024];
        uint32_t length = 0;
        long offset;
        FILE *fp;
        size_t got;

        snprintf(path, sizeof path, "%s/COSMO%d.%s",
                 datadir, episode, extensions[g]);

        offset = find_entry(path, entry_name, &length);
        if (offset < 0) continue;

        fp = fopen(path, "rb");
        if (!fp) continue;
        if (fseek(fp, offset, SEEK_SET) != 0) { fclose(fp); continue; }

        got = fread(dest, 1, (length < max) ? length : max, fp);
        fclose(fp);

        return got;
    }

    return 0;
}

bool group_episode_present(const char *datadir, int episode)
{
    static const char *const extensions[] = {"STN", "VOL"};

    for (size_t g = 0; g < sizeof extensions / sizeof extensions[0]; g++) {
        char path[1024];
        FILE *fp;

        snprintf(path, sizeof path, "%s/COSMO%d.%s",
                 datadir, episode, extensions[g]);

        fp = fopen(path, "rb");
        if (!fp) return false;
        fclose(fp);
    }

    return true;
}
