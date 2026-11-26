#ifndef COSMO_MP3_STREAM_VITA_H
#define COSMO_MP3_STREAM_VITA_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VitaMusicStream VitaMusicStream;

typedef struct VitaMusicStats {
    int buffered_ms;
    int prebuffer_ms;
    int first_mix_ms;
    int underruns;
    int read_us;
    int setup_us;
    int decode_us;
    int convert_us;
    int allocation_us;
    int source_rate;
    int source_channels;
    bool failed;
} VitaMusicStats;

VitaMusicStream *vita_music_start(const char *path);
void vita_music_stop(VitaMusicStream *music);
void vita_music_render(VitaMusicStream *music, int16_t *out, int frames);
VitaMusicStats vita_music_stats(const VitaMusicStream *music);

#ifdef __cplusplus
}
#endif

#endif
