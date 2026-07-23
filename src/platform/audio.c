/*
 * audio.c -- PC speaker output.
 *
 * The speaker was a one-bit device driven by PIT channel 2 as a square wave
 * generator, so reproducing it is a matter of reading back the divisor the
 * game programmed and emitting a square wave at the frequency it implies.
 *
 * The phase carries across frequency changes on purpose. The game rewrites the
 * divisor every timer service call -- 140 times a second -- and restarting the
 * waveform each time would add a click at every step of every sound effect.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "cosmo/audio.h"
#include "cosmo/hardware.h"

#define SAMPLE_RATE 48000

/*
 * A small buffer matters here. Each sound sample lasts one timer service call,
 * 1/140 s or about 7.1 ms, so a buffer longer than that would step over whole
 * samples of an effect and flatten it.
 */
#define BUFFER_FRAMES 128

/*
 * The speaker was loud and square. A quarter of full scale keeps the harmonics
 * honest without being painful through headphones.
 */
#define AMPLITUDE 8192

static SDL_AudioStream *stream;
static double phase;          /* 0..1 across one square wave cycle */

static FILE *wav_file;
static uint32_t wav_frames;
static const char *wav_path;

/* ------------------------------------------------------------------------ */
/* WAV capture                                                              */
/* ------------------------------------------------------------------------ */

static void put_le32(FILE *fp, uint32_t v)
{
    fputc((int)(v & 0xFF), fp);
    fputc((int)((v >> 8) & 0xFF), fp);
    fputc((int)((v >> 16) & 0xFF), fp);
    fputc((int)((v >> 24) & 0xFF), fp);
}

static void put_le16(FILE *fp, uint16_t v)
{
    fputc((int)(v & 0xFF), fp);
    fputc((int)((v >> 8) & 0xFF), fp);
}

static void wav_open(const char *path)
{
    wav_file = fopen(path, "wb");
    if (!wav_file) {
        fprintf(stderr, "cosmo: cannot open %s for audio capture\n", path);
        return;
    }

    /* Sizes are patched in on close, once the length is known. */
    fwrite("RIFF", 1, 4, wav_file);
    put_le32(wav_file, 0);
    fwrite("WAVEfmt ", 1, 8, wav_file);
    put_le32(wav_file, 16);        /* PCM header size */
    put_le16(wav_file, 1);         /* format: PCM */
    put_le16(wav_file, 1);         /* channels */
    put_le32(wav_file, SAMPLE_RATE);
    put_le32(wav_file, SAMPLE_RATE * 2);  /* byte rate */
    put_le16(wav_file, 2);         /* block align */
    put_le16(wav_file, 16);        /* bits per sample */
    fwrite("data", 1, 4, wav_file);
    put_le32(wav_file, 0);
}

/*
 * Patch the two length fields. Done after every write, not just on close: a
 * capture is usually ended by killing the game, and a file that only becomes
 * valid on a clean exit is useless for exactly the runs worth recording.
 */
static void wav_update_sizes(void)
{
    long here;

    if (!wav_file) return;

    here = ftell(wav_file);

    fseek(wav_file, 4, SEEK_SET);
    put_le32(wav_file, 36 + wav_frames * 2);
    fseek(wav_file, 40, SEEK_SET);
    put_le32(wav_file, wav_frames * 2);

    fseek(wav_file, here, SEEK_SET);
    fflush(wav_file);
}

static void wav_close(void)
{
    if (!wav_file) return;

    wav_update_sizes();
    fclose(wav_file);
    wav_file = NULL;

    printf("cosmo: wrote %u audio frames to %s\n", wav_frames, wav_path);
    fflush(stdout);
}

void audio_record_to(const char *path)
{
    wav_path = path;
}

/* ------------------------------------------------------------------------ */
/* Generation                                                               */
/* ------------------------------------------------------------------------ */

static void render(int16_t *out, int frames)
{
    uint32_t state = speaker_state();
    uint16_t divisor = (uint16_t)(state & 0xFFFF);
    bool gated_on = (state & (1u << 16)) != 0;
    double step;

    if (!gated_on || divisor == 0) {
        /*
         * Silence. The phase is deliberately left where it is so a sound that
         * resumes picks up mid-cycle rather than clicking.
         */
        memset(out, 0, (size_t)frames * sizeof *out);
        return;
    }

    step = (PIT_BASE_HZ / (double)divisor) / (double)SAMPLE_RATE;

    for (int i = 0; i < frames; i++) {
        out[i] = (phase < 0.5) ? AMPLITUDE : -AMPLITUDE;

        phase += step;
        if (phase >= 1.0) phase -= 1.0;
    }
}

static void SDLCALL audio_callback(void *userdata, SDL_AudioStream *audio_stream,
                                   int additional_amount, int total_amount)
{
    int16_t buffer[BUFFER_FRAMES];

    (void)userdata;
    (void)total_amount;

    while (additional_amount > 0) {
        int frames = additional_amount / (int)sizeof(int16_t);

        if (frames > BUFFER_FRAMES) frames = BUFFER_FRAMES;
        if (frames <= 0) break;

        render(buffer, frames);
        SDL_PutAudioStreamData(audio_stream, buffer,
                               frames * (int)sizeof(int16_t));

        if (wav_file) {
            fwrite(buffer, sizeof(int16_t), (size_t)frames, wav_file);
            wav_frames += (uint32_t)frames;
            wav_update_sizes();
        }

        additional_amount -= frames * (int)sizeof(int16_t);
    }
}

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                */
/* ------------------------------------------------------------------------ */

bool audio_init(void)
{
    SDL_AudioSpec spec;

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "cosmo: no audio (%s); running silently\n",
                SDL_GetError());
        return false;
    }

    /*
     * Ask for a short device buffer. SDL will not necessarily honour it, but
     * the callback fills in BUFFER_FRAMES chunks either way, so a frequency
     * change is never quantised more coarsely than that.
     */
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "256");

    spec.format = SDL_AUDIO_S16;
    spec.channels = 1;
    spec.freq = SAMPLE_RATE;

    if (wav_path) wav_open(wav_path);

    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                       &spec, audio_callback, NULL);
    if (!stream) {
        fprintf(stderr, "cosmo: no audio device (%s); running silently\n",
                SDL_GetError());
        return false;
    }

    SDL_ResumeAudioStreamDevice(stream);
    return true;
}

void audio_shutdown(void)
{
    if (stream) {
        SDL_DestroyAudioStream(stream);
        stream = NULL;
    }
    wav_close();
}
