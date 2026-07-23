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

#endif /* COSMO_AUDIO_H */
