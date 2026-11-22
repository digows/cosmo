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
#define MUSIC_FADE_FRAMES (SAMPLE_RATE * 300 / 1000)
#define IMF_TICK_RATE 560.0

static SDL_AudioStream *stream;
#ifdef COSMO_VITA_APP
static SDL_AudioDeviceID audio_device;
#endif
static double phase;          /* 0..1 across one square wave cycle */

static FILE *wav_file;
static uint32_t wav_frames;
static const char *wav_path;
static int16_t *mp3_samples;
static uint64_t mp3_frames;
static uint64_t mp3_cursor;
static uint8_t *imf_music_data;
static size_t imf_music_size;
static size_t imf_music_cursor;
static uint32_t imf_music_tick;
static uint32_t imf_music_next_due;
static double imf_frames_to_tick;
static int16_t *mp3_sound_samples;
static uint64_t mp3_sound_frames;
static uint64_t mp3_sound_cursor;
static struct {
    char path[1024];
    int16_t *samples;
    uint64_t frames;
} mp3_sound_cache[SFX_CACHE_LIMIT];
static size_t mp3_sound_cache_count;
static SDL_AtomicInt sound_volume_percent = { 100 };
static SDL_AtomicInt music_volume_percent = { 100 };
static SDL_AtomicInt music_paused;
static int music_fade_frames = MUSIC_FADE_FRAMES; /* Audio callback only. */

static void render_imf(int16_t *out, int frames)
{
    int rendered = 0;
    const double frames_per_tick = (double)SAMPLE_RATE / IMF_TICK_RATE;

    while (rendered < frames) {
        if (imf_frames_to_tick <= 0.0) {
            while (imf_music_cursor + 4 <= imf_music_size &&
                   imf_music_next_due <= imf_music_tick) {
                const uint8_t *event = imf_music_data + imf_music_cursor;
                uint16_t delay = (uint16_t)(event[2] | ((uint16_t)event[3] << 8));
                opl_write(event[0], event[1]);
                imf_music_cursor += 4;
                imf_music_next_due = imf_music_tick + delay;
            }

            ++imf_music_tick;
            if (imf_music_cursor >= imf_music_size) {
                imf_music_cursor = 0;
                imf_music_tick = 0;
                imf_music_next_due = 0;
            }
            imf_frames_to_tick += frames_per_tick;
        }

        int chunk = (int)imf_frames_to_tick;
        if ((double)chunk < imf_frames_to_tick) ++chunk;
        if (chunk > frames - rendered) chunk = frames - rendered;
        if (chunk < 1) chunk = 1;
        opl_generate(out + rendered, chunk);
        rendered += chunk;
        imf_frames_to_tick -= chunk;
    }
}

#if defined(_WIN32) || defined(__ANDROID__) || defined(COSMO_VITA_APP)
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
    int sound_volume = SDL_GetAtomicInt(&sound_volume_percent);
    int music_volume = SDL_GetAtomicInt(&music_volume_percent);
    bool paused = SDL_GetAtomicInt(&music_paused) != 0;
    bool playing_mp3 = mp3_samples && mp3_frames;
    bool playing_imf = imf_music_data && imf_music_size >= 4;
    float music_gain = music_volume / 100.0f;

    if (playing_mp3) {
        music_gain *= MP3_MUSIC_GAIN;
        for (int i = 0; i < frames; ++i) {
            out[i] = mp3_samples[mp3_cursor++];
            if (mp3_cursor >= mp3_frames) mp3_cursor = 0;
        }
    } else if (playing_imf) {
        render_imf(out, frames);
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
        if (paused && music_fade_frames > 0) --music_fade_frames;
        if (!paused && music_fade_frames < MUSIC_FADE_FRAMES) ++music_fade_frames;
        float fade_gain = (float)music_fade_frames / MUSIC_FADE_FRAMES;
        int32_t mixed = (int32_t)(out[i] * music_gain * fade_gain)
                      + (int32_t)speaker[i] * sound_volume / 100;

        if (mp3_sound_samples && mp3_sound_cursor < mp3_sound_frames)
            mixed += (int32_t)((float)mp3_sound_samples[mp3_sound_cursor++] * MP3_MUSIC_GAIN * sound_volume / 100.0f);

        if (mixed > 32767) mixed = 32767;
        if (mixed < -32768) mixed = -32768;

        out[i] = (int16_t)mixed;
    }
}

void audio_set_music_paused(bool paused)
{
    SDL_SetAtomicInt(&music_paused, paused);
}

void audio_set_volumes(int sound_percent, int music_percent)
{
    if (sound_percent < 0) sound_percent = 0;
    if (sound_percent > 100) sound_percent = 100;
    if (music_percent < 0) music_percent = 0;
    if (music_percent > 100) music_percent = 100;
    SDL_SetAtomicInt(&sound_volume_percent, sound_percent);
    SDL_SetAtomicInt(&music_volume_percent, music_percent);
}

bool audio_music_play_mp3(const char *path)
{
#if defined(_WIN32) || defined(__ANDROID__) || defined(COSMO_VITA_APP)
    int16_t *decoded = NULL;
    uint64_t frames = 0;
    int16_t *old;
    if (!path || !cosmo_mp3_decode(path,&decoded,&frames) || !decoded || !frames) {
        free(decoded);
        SDL_Log("Cosmo audio: could not decode MP3 music%s%s",
                path ? " " : "", path ? path : "");
        return false;
    }
    if (stream) SDL_LockAudioStream(stream);
    old = mp3_samples;
    mp3_samples = decoded;
    mp3_frames = frames;
    mp3_cursor = 0;
    uint8_t *old_imf = imf_music_data;
    imf_music_data = NULL;
    imf_music_size = imf_music_cursor = 0;
    imf_music_tick = imf_music_next_due = 0;
    imf_frames_to_tick = 0.0;
    if (stream) SDL_UnlockAudioStream(stream);
    free(old);
    free(old_imf);
    SDL_Log("Cosmo audio: loaded MP3 music (%llu frames)",
            (unsigned long long)frames);
    return true;
#else
    (void)path;
    return false;
#endif
}

bool audio_music_play_imf(const void *data, size_t size)
{
    uint8_t *copy;
    int16_t *old_mp3;
    uint8_t *old_imf;

    if (!data || size < 4 || (size & 3u)) return false;
    copy = malloc(size);
    if (!copy) return false;
    memcpy(copy, data, size);

    if (stream) SDL_LockAudioStream(stream);
    old_mp3 = mp3_samples;
    old_imf = imf_music_data;
    mp3_samples = NULL;
    mp3_frames = mp3_cursor = 0;
    imf_music_data = copy;
    imf_music_size = size;
    imf_music_cursor = 0;
    imf_music_tick = 0;
    imf_music_next_due = 0;
    imf_frames_to_tick = 0.0;
    if (stream) SDL_UnlockAudioStream(stream);
    free(old_mp3);
    free(old_imf);
    opl_reset();
    return true;
}

void audio_music_stop_mp3(void)
{
    int16_t *old;
    uint8_t *old_imf;
    if (stream) SDL_LockAudioStream(stream);
    old = mp3_samples;
    old_imf = imf_music_data;
    mp3_samples = NULL;
    mp3_frames = mp3_cursor = 0;
    imf_music_data = NULL;
    imf_music_size = imf_music_cursor = 0;
    imf_music_tick = imf_music_next_due = 0;
    imf_frames_to_tick = 0.0;
    if (stream) SDL_UnlockAudioStream(stream);
    free(old);
    free(old_imf);
    if (old_imf) opl_reset();
}

#if defined(_WIN32) || defined(__ANDROID__) || defined(COSMO_VITA_APP)
static int cache_sound_asset(const char *path)
{
    if (!path) return -1;
    for (size_t i = 0; i < mp3_sound_cache_count; ++i) {
        if (!SDL_strcmp(mp3_sound_cache[i].path,path)) return (int)i;
    }
    if (mp3_sound_cache_count >= SFX_CACHE_LIMIT) return -1;
    int16_t *decoded = NULL;
    uint64_t frames = 0;
    const char *extension = strrchr(path, '.');
    bool is_wav = extension && SDL_strcasecmp(extension, ".wav") == 0;
    bool decoded_ok;
    if (is_wav) {
        decoded_ok = decode_sfx_wav(path, &decoded, &frames);
    } else {
#if defined(_WIN32) || defined(__ANDROID__) || defined(COSMO_VITA_APP)
        decoded_ok = cosmo_mp3_decode(path, &decoded, &frames);
#else
        decoded_ok = false;
#endif
    }
    if (!decoded_ok || !decoded || !frames) {
        free(decoded);
        return -1;
    }
    size_t i = mp3_sound_cache_count++;
    SDL_strlcpy(mp3_sound_cache[i].path,path,sizeof mp3_sound_cache[i].path);
    mp3_sound_cache[i].samples = decoded;
    mp3_sound_cache[i].frames = frames;
    return (int)i;
}
#endif

bool audio_sound_preload_asset(const char *path)
{
#if defined(_WIN32) || defined(__ANDROID__) || defined(COSMO_VITA_APP)
    return cache_sound_asset(path) >= 0;
#else
    (void)path;
    return false;
#endif
}

bool audio_sound_play_asset(const char *path)
{
#if defined(_WIN32) || defined(__ANDROID__) || defined(COSMO_VITA_APP)
    int asset = cache_sound_asset(path);
    if (asset < 0) return false;
    if (stream) SDL_LockAudioStream(stream);
    mp3_sound_samples = mp3_sound_cache[asset].samples;
    mp3_sound_frames = mp3_sound_cache[asset].frames;
    mp3_sound_cursor = 0;
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

#ifdef COSMO_VITA_APP
static bool audio_open_vita_stream(const SDL_AudioSpec *source_spec)
{
    SDL_AudioSpec device_request;
    SDL_AudioSpec device_spec;

    device_request.format = SDL_AUDIO_S16;
    device_request.channels = 1;
    device_request.freq = 48000;
    audio_device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                       &device_request);
    if (!audio_device) return false;

    if (!SDL_GetAudioDeviceFormat(audio_device, &device_spec, NULL)) goto fail;
    stream = SDL_CreateAudioStream(source_spec, &device_spec);
    if (!stream) goto fail;
    if (!SDL_BindAudioStream(audio_device, stream)) goto fail;
    if (!SDL_SetAudioStreamGetCallback(stream, audio_callback, NULL)) goto fail;

    SDL_Log("Cosmo audio: Vita output %d Hz, %d channel(s)",
            device_spec.freq, device_spec.channels);
    if (!SDL_ResumeAudioDevice(audio_device)) goto fail;
    return true;

fail:
    if (stream) {
        SDL_DestroyAudioStream(stream);
        stream = NULL;
    }
    SDL_CloseAudioDevice(audio_device);
    audio_device = 0;
    return false;
}
#endif

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
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "1024");
#else
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "256");
#endif

    spec.format = SDL_AUDIO_S16;
    spec.channels = 1;
    spec.freq = SAMPLE_RATE;

    if (wav_path) wav_open(wav_path);

#ifdef COSMO_VITA_APP
    if (!audio_open_vita_stream(&spec)) {
#else
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                       &spec, audio_callback, NULL);
    if (!stream) {
#endif
        fprintf(stderr, "cosmo: no audio device (%s); running silently\n",
                SDL_GetError());
        return false;
    }

#ifndef COSMO_VITA_APP
    SDL_ResumeAudioStreamDevice(stream);
#endif
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
#ifdef COSMO_VITA_APP
    if (audio_device) {
        SDL_CloseAudioDevice(audio_device);
        audio_device = 0;
    }
#endif
    wav_close();
    for (size_t i = 0; i < mp3_sound_cache_count; ++i)
        free(mp3_sound_cache[i].samples);
    memset(mp3_sound_cache,0,sizeof mp3_sound_cache);
    mp3_sound_cache_count = 0;
}
