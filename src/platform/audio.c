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
#include "cosmo/opl.h"

/*
 * The YM3812's own rate. Generating there and letting SDL resample to the
 * device keeps the synthesis on exactly the timing the register values assume.
 */
#define SAMPLE_RATE ((int)OPL_SAMPLE_RATE)

/*
 * A small buffer matters here. Each sound sample lasts one timer service call,
 * 1/140 s or about 7.1 ms, so a buffer longer than that would step over whole
 * samples of an effect and flatten it.
 */
#define BUFFER_FRAMES 128
#define SFX_CACHE_LIMIT 128

/*
 * The speaker was loud and square. A quarter of full scale keeps the harmonics
 * honest without being painful through headphones.
 */
#define AMPLITUDE 6000
#define MP3_MUSIC_GAIN 0.50f

static SDL_AudioStream *stream;
static double phase;          /* 0..1 across one square wave cycle */

static FILE *wav_file;
static uint32_t wav_frames;
static const char *wav_path;
static int16_t *mp3_samples;
static uint64_t mp3_frames;
static uint64_t mp3_cursor;
static int16_t *mp3_sound_samples;
static uint64_t mp3_sound_frames;
static uint64_t mp3_sound_cursor;
static struct {
    char path[1024];
    int16_t *samples;
    uint64_t frames;
} mp3_sound_cache[SFX_CACHE_LIMIT];
static size_t mp3_sound_cache_count;

#ifdef _WIN32
extern bool cosmo_mp3_decode(const char *path, int16_t **samples, uint64_t *frames);
#endif

static uint16_t wav_u16(const unsigned char *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t wav_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Read mono 16-bit PCM WAV and resample it to the engine's mix rate. */
static bool decode_sfx_wav(const char *path, int16_t **samples, uint64_t *frames)
{
    FILE *fp = fopen(path, "rb");
    unsigned char header[12], chunk[8], fmt[16];
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0, data_size = 0;
    long data_offset = 0;
    int16_t *source = NULL, *output = NULL;
    uint64_t source_frames, output_frames;
    bool have_fmt = false, have_data = false, ok = false;

    if (!fp) return false;
    if (fread(header, 1, sizeof header, fp) != sizeof header ||
        memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4)) goto done;
    while (fread(chunk, 1, sizeof chunk, fp) == sizeof chunk) {
        uint32_t size = wav_u32(chunk + 4);
        long payload = ftell(fp);
        if (!memcmp(chunk, "fmt ", 4) && size >= sizeof fmt) {
            if (fread(fmt, 1, sizeof fmt, fp) != sizeof fmt) goto done;
            format = wav_u16(fmt); channels = wav_u16(fmt + 2);
            rate = wav_u32(fmt + 4); bits = wav_u16(fmt + 14);
            have_fmt = true;
        } else if (!memcmp(chunk, "data", 4)) {
            data_offset = payload; data_size = size; have_data = true;
        }
        if (fseek(fp, payload + size + (size & 1u), SEEK_SET) != 0) goto done;
        if (have_fmt && have_data) break;
    }
    if (!have_fmt || !have_data || format != 1 || channels != 1 || bits != 16 ||
        !rate || data_size < 2 || (data_size & 1u)) goto done;
    source_frames = data_size / 2;
    if (source_frames > UINT32_MAX / 2) goto done;
    source = (int16_t *)malloc((size_t)data_size);
    if (!source || fseek(fp, data_offset, SEEK_SET) != 0 ||
        fread(source, 1, data_size, fp) != data_size) goto done;
    output_frames = (source_frames * (uint64_t)SAMPLE_RATE + rate - 1) / rate;
    if (!output_frames || output_frames > SIZE_MAX / sizeof *output) goto done;
    output = (int16_t *)malloc((size_t)output_frames * sizeof *output);
    if (!output) goto done;
    for (uint64_t i = 0; i < output_frames; ++i) {
        double at = (double)i * rate / SAMPLE_RATE;
        uint64_t a = (uint64_t)at;
        uint64_t b = a + 1 < source_frames ? a + 1 : a;
        double fraction = at - (double)a;
        double value = source[a] + (source[b] - source[a]) * fraction;
        if (value > 32767.0) value = 32767.0;
        if (value < -32768.0) value = -32768.0;
        output[i] = (int16_t)value;
    }
    *samples = output; *frames = output_frames; output = NULL; ok = true;
done:
    free(source); free(output); fclose(fp); return ok;
}

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

static void render_speaker(int16_t *out, int frames)
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

static void render(int16_t *out, int frames)
{
    int16_t speaker[BUFFER_FRAMES];

    if (mp3_samples && mp3_frames) {
        for (int i = 0; i < frames; ++i) {
            out[i] = (int16_t)((float)mp3_samples[mp3_cursor++] * MP3_MUSIC_GAIN);
            if (mp3_cursor >= mp3_frames) mp3_cursor = 0;
        }
    } else {
        opl_generate(out, frames);
    }
    render_speaker(speaker, frames);

    /*
     * The AdLib and the speaker were separate pieces of hardware heard
     * together, so they are simply summed. The clamp is a backstop for the
     * rare moment when a loud effect lands on a loud passage; the levels are
     * chosen so it is not reached in normal play.
     */
    for (int i = 0; i < frames; i++) {
        int32_t mixed = (int32_t)out[i] + speaker[i];

        if (mp3_sound_samples && mp3_sound_cursor < mp3_sound_frames)
            mixed += (int32_t)((float)mp3_sound_samples[mp3_sound_cursor++] * MP3_MUSIC_GAIN);

        if (mixed > 32767) mixed = 32767;
        if (mixed < -32768) mixed = -32768;

        out[i] = (int16_t)mixed;
    }
}

bool audio_music_play_mp3(const char *path)
{
#ifdef _WIN32
    int16_t *decoded = NULL;
    uint64_t frames = 0;
    int16_t *old;
    if (!path || !cosmo_mp3_decode(path,&decoded,&frames) || !decoded || !frames) {
        free(decoded);
        return false;
    }
    if (stream) SDL_LockAudioStream(stream);
    old = mp3_samples;
    mp3_samples = decoded;
    mp3_frames = frames;
    mp3_cursor = 0;
    if (stream) SDL_UnlockAudioStream(stream);
    free(old);
    return true;
#else
    (void)path;
    return false;
#endif
}

void audio_music_stop_mp3(void)
{
    int16_t *old;
    if (stream) SDL_LockAudioStream(stream);
    old = mp3_samples;
    mp3_samples = NULL;
    mp3_frames = mp3_cursor = 0;
    if (stream) SDL_UnlockAudioStream(stream);
    free(old);
}

bool audio_sound_play_asset(const char *path)
{
#ifdef _WIN32
    if (!path) return false;
    for (size_t i = 0; i < mp3_sound_cache_count; ++i) {
        if (!SDL_strcmp(mp3_sound_cache[i].path,path)) {
            if (stream) SDL_LockAudioStream(stream);
            mp3_sound_samples = mp3_sound_cache[i].samples;
            mp3_sound_frames = mp3_sound_cache[i].frames;
            mp3_sound_cursor = 0;
            if (stream) SDL_UnlockAudioStream(stream);
            return true;
        }
    }
    if (mp3_sound_cache_count >= SFX_CACHE_LIMIT) return false;
    int16_t *decoded = NULL;
    uint64_t frames = 0;
    const char *extension = strrchr(path, '.');
    bool is_wav = extension && SDL_strcasecmp(extension, ".wav") == 0;
    bool decoded_ok = is_wav ? decode_sfx_wav(path, &decoded, &frames)
                             : cosmo_mp3_decode(path, &decoded, &frames);
    if (!decoded_ok || !decoded || !frames) {
        free(decoded);
        return false;
    }
    SDL_strlcpy(mp3_sound_cache[mp3_sound_cache_count].path,path,
                sizeof mp3_sound_cache[mp3_sound_cache_count].path);
    mp3_sound_cache[mp3_sound_cache_count].samples = decoded;
    mp3_sound_cache[mp3_sound_cache_count].frames = frames;
    if (stream) SDL_LockAudioStream(stream);
    mp3_sound_samples = decoded;
    mp3_sound_frames = frames;
    mp3_sound_cursor = 0;
    ++mp3_sound_cache_count;
    if (stream) SDL_UnlockAudioStream(stream);
    return true;
#else
    (void)path;
    return false;
#endif
}

void audio_sound_stop_mp3(void)
{
    if (stream) SDL_LockAudioStream(stream);
    mp3_sound_samples = NULL;
    mp3_sound_frames = mp3_sound_cursor = 0;
    if (stream) SDL_UnlockAudioStream(stream);
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
    audio_music_stop_mp3();
    audio_sound_stop_mp3();
    if (stream) {
        SDL_DestroyAudioStream(stream);
        stream = NULL;
    }
    wav_close();
    for (size_t i = 0; i < mp3_sound_cache_count; ++i)
        free(mp3_sound_cache[i].samples);
    memset(mp3_sound_cache,0,sizeof mp3_sound_cache);
    mp3_sound_cache_count = 0;
}
