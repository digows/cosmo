/*
 * audio.h -- Sound output.
 *
 * The game's sound effects are PC speaker square waves: each effect is a list
 * of PIT divisors, one played per timer service call. The speaker itself is
 * modelled in ports.c, which tracks the divisor the game programs into PIT
 * channel 2 and whether port 0x61 has the output gated on. This turns that
 * state into samples.
 */

#ifndef COSMO_AUDIO_H
#define COSMO_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Open the audio device. Returns false if none is available, which is not
 * fatal -- the game runs silently, as it did on a PC with the speaker
 * disconnected.
 */
bool audio_init(void);
void audio_shutdown(void);

/*
 * Record everything produced to a WAV file, for checking the output without
 * listening to it. Call before audio_init(). Driven by COSMO_AUDIO_WAV.
 */
void audio_record_to(const char *path);

/* Play a decoded MP3 as the active music track. False means no decoder or
 * the file could not be decoded; callers should use original IMF playback. */
bool audio_music_play_mp3(const char *path);
void audio_music_stop_mp3(void);
/* Fade music to silence while the game is paused. */
void audio_set_music_paused(bool paused);

/* Play a one-shot WAV or MP3 effect. False means use the original effect. */
bool audio_sound_play_asset(const char *path);
void audio_sound_stop_mp3(void);
void audio_set_volumes(int sound_percent, int music_percent);

#endif /* COSMO_AUDIO_H */
