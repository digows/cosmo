#include <psp2/audiodec.h>
#include <psp2/sysmodule.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

#include "cosmo/opl.h"
#include "mp3_stream_vita.h"

struct VitaMusicStream {
    static constexpr unsigned capacity = 32768;
    static constexpr unsigned prebuffer = OPL_SAMPLE_RATE * 150 / 1000;
    std::atomic<unsigned> read{0}, written{0};
    std::atomic<bool> cancelled{false}, failed{false};
    std::atomic<int> ready_ms{-1}, first_mix_ms{-1}, underruns{0};
    std::atomic<int> read_us{0}, setup_us{0}, decode_us{0}, convert_us{0};
    std::atomic<int> source_rate{0}, source_channels{0};
    int allocation_us = 0;
    int16_t pcm[capacity];
    SDL_Thread *worker = nullptr;
    char *path = nullptr;
    uint64_t started_ns = 0;
    bool playing = false; // Audio callback only.
};

namespace {

struct mp3_header {
    uint32_t version;
    uint32_t channels;
    uint32_t sample_rate;
    size_t frame_size;
};

std::mutex decoder_mutex;
bool decoder_library_ready;

bool parse_header(const uint8_t *data, size_t size, size_t offset,
                  mp3_header *header)
{
    static const uint16_t mpeg1_bitrates[] = {
        0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
    };
    static const uint16_t mpeg2_bitrates[] = {
        0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0
    };
    static const uint32_t sample_rates[4][3] = {
        { 11025, 12000, 8000 },
        { 0, 0, 0 },
        { 22050, 24000, 16000 },
        { 44100, 48000, 32000 }
    };

    if (offset > size || size - offset < 4 ||
        data[offset] != 0xff || (data[offset + 1] & 0xe0) != 0xe0) {
        return false;
    }

    const uint32_t version = (data[offset + 1] >> 3) & 3;
    const uint32_t layer = (data[offset + 1] >> 1) & 3;
    const uint32_t bitrate_index = (data[offset + 2] >> 4) & 15;
    const uint32_t rate_index = (data[offset + 2] >> 2) & 3;
    if (version == 1 || layer != 1 || !bitrate_index || bitrate_index == 15 ||
        rate_index == 3) {
        return false;
    }

    const uint32_t sample_rate = sample_rates[version][rate_index];
    const uint32_t bitrate = version == 3 ? mpeg1_bitrates[bitrate_index]
                                           : mpeg2_bitrates[bitrate_index];
    const size_t frame_size =
        ((version == 3 ? 144000u : 72000u) * bitrate) / sample_rate +
        ((data[offset + 2] >> 1) & 1);
    if (frame_size < 4 || frame_size > size - offset) return false;

    header->version = version;
    header->channels = ((data[offset + 3] >> 6) & 3) == 3 ? 1 : 2;
    header->sample_rate = sample_rate;
    header->frame_size = frame_size;
    return true;
}

bool find_first_frame(const uint8_t *data, size_t size, size_t *offset,
                      mp3_header *header)
{
    size_t start = 0;
    if (size >= 10 && data[0] == 'I' && data[1] == 'D' && data[2] == '3') {
        const uint8_t *tag = data + 6;
        if ((tag[0] | tag[1] | tag[2] | tag[3]) & 0x80) return false;
        start = 10 + (static_cast<size_t>(tag[0]) << 21) +
                (static_cast<size_t>(tag[1]) << 14) +
                (static_cast<size_t>(tag[2]) << 7) + tag[3];
        if (start > size) return false;
    }

    const size_t limit = start + (size - start < 8192 ? size - start : 8192);
    for (size_t i = start; i + 4 <= size && i < limit; ++i) {
        mp3_header candidate;
        if (!parse_header(data, size, i, &candidate)) continue;

        mp3_header next;
        const size_t next_offset = i + candidate.frame_size;
        if (next_offset + 4 <= size &&
            (!parse_header(data, size, next_offset, &next) ||
             next.version != candidate.version ||
             next.sample_rate != candidate.sample_rate)) {
            continue;
        }

        *offset = i;
        *header = candidate;
        return true;
    }
    return false;
}

bool init_decoder_library()
{
    if (decoder_library_ready) return true;

    const SceInt32 module_result =
        sceSysmoduleLoadModule(SCE_SYSMODULE_AUDIOCODEC);
    if (module_result < 0) {
        SDL_Log("Cosmo audio: decoder module load returned 0x%08x; "
                "trying decoder initialization",
                static_cast<uint32_t>(module_result));
    }

    SceAudiodecInitParam param = {};
    param.mp3.size = sizeof(SceAudiodecInitStreamParam);
    param.mp3.totalStreams = 2;

    const SceInt32 result =
        sceAudiodecInitLibrary(SCE_AUDIODEC_TYPE_MP3, &param);
    if (result < 0 &&
        result != static_cast<SceInt32>(SCE_AUDIODEC_ERROR_ALREADY_INITIALIZED)) {
        SDL_Log("Cosmo audio: could not initialize Vita MP3 decoder (0x%08x)",
                static_cast<uint32_t>(result));
        return false;
    }

    decoder_library_ready = true;
    return true;
}

struct decoder_handle {
    SceAudiodecCtrl control = {};
    bool created = false;

    ~decoder_handle()
    {
        if (created) sceAudiodecDeleteDecoder(&control);
    }
};

template <typename Sink, typename Cancelled>
bool decode_frames(const uint8_t *data, size_t size, size_t offset,
                   const mp3_header &header, Sink sink, Cancelled cancelled,
                   uint64_t *setup_ns = nullptr, uint64_t *decode_ns = nullptr)
{
    const uint64_t setup_start = setup_ns ? SDL_GetTicksNS() : 0;
    SceAudiodecInfo info = {};
    info.mp3.size = sizeof(SceAudiodecInfoMp3);
    info.mp3.ch = header.channels;
    info.mp3.version = header.version;

    decoder_handle decoder;
    decoder.control.size = sizeof(decoder.control);
    decoder.control.pInfo = &info;
    decoder.control.maxEsSize = SCE_AUDIODEC_MP3_MAX_ES_SIZE;
    decoder.control.maxPcmSize =
        SCE_AUDIODEC_MP3_MAX_SAMPLES * SCE_AUDIODEC_MP3_MAX_CH_IN_DECODER *
        sizeof(int16_t);
    decoder.control.wordLength = SCE_AUDIODEC_WORD_LENGTH_16BITS;

    const SceInt32 create_result =
        sceAudiodecCreateDecoder(&decoder.control, SCE_AUDIODEC_TYPE_MP3);
    if (setup_ns) *setup_ns += SDL_GetTicksNS() - setup_start;
    if (create_result < 0) {
        SDL_Log("Cosmo audio: could not create Vita MP3 decoder (0x%08x)",
                static_cast<uint32_t>(create_result));
        return false;
    }
    decoder.created = true;

    int16_t pcm[SCE_AUDIODEC_MP3_MAX_SAMPLES *
                SCE_AUDIODEC_MP3_MAX_CH_IN_DECODER];
    size_t consecutive_errors = 0;
    size_t decoded_frames = 0;

    while (offset + 4 <= size && !cancelled()) {
        mp3_header current;
        if (!parse_header(data, size, offset, &current)) {
            if (++consecutive_errors >= 64) break;
            ++offset;
            continue;
        }

        const size_t available = size - offset;
        decoder.control.pEs = const_cast<SceUInt8 *>(data + offset);
        decoder.control.inputEsSize = 0;
        decoder.control.maxEsSize = static_cast<SceUInt32>(
            available < SCE_AUDIODEC_MP3_MAX_ES_SIZE
                ? available
                : SCE_AUDIODEC_MP3_MAX_ES_SIZE);
        decoder.control.pPcm = pcm;
        decoder.control.outputPcmSize = 0;
        decoder.control.maxPcmSize = sizeof(pcm);

        const uint64_t decode_start = decode_ns ? SDL_GetTicksNS() : 0;
        const SceInt32 result = sceAudiodecDecode(&decoder.control);
        if (decode_ns) *decode_ns += SDL_GetTicksNS() - decode_start;
        const size_t consumed = decoder.control.inputEsSize;
        const size_t output_bytes = decoder.control.outputPcmSize;
        if (result < 0 || !consumed ||
            consumed > available || output_bytes > sizeof(pcm) ||
            output_bytes % sizeof(int16_t) != 0 ||
            (output_bytes / sizeof(int16_t)) % header.channels != 0) {
            if (++consecutive_errors >= 64) break;
            ++offset;
            continue;
        }

        consecutive_errors = 0;
        offset += consumed;
        const size_t sample_count = output_bytes / sizeof(int16_t);
        if (sample_count) {
            if (!sink(pcm, sample_count)) return false;
            ++decoded_frames;
        }
    }

    return decoded_frames > 0;
}

bool publish_pcm(VitaMusicStream &music, const int16_t *pcm, unsigned count)
{
    unsigned write = music.written.load(std::memory_order_relaxed);
    while (count && !music.cancelled.load(std::memory_order_relaxed)) {
        const unsigned used = write - music.read.load(std::memory_order_acquire);
        const unsigned position = write % music.capacity;
        const unsigned n = std::min({count, music.capacity - used,
                                     music.capacity - position});
        if (!n) {
            SDL_Delay(2);
            continue;
        }
        SDL_memcpy(music.pcm + position, pcm, n * sizeof(*pcm));
        write += n;
        music.written.store(write, std::memory_order_release);
        pcm += n;
        count -= n;
    }
    return count == 0;
}

// Resample while the MP3 frame is still in scratch storage. The fractional
// position carries across decoder frames, so there is no per-frame rounding.
struct PcmConverter {
    static constexpr unsigned block_size = 512;
    uint32_t input_rate;
    uint32_t channels;
    int16_t previous = 0;
    unsigned position = 0;
    bool have_previous = false;

    bool push(VitaMusicStream &music, const int16_t *pcm, size_t samples,
              uint64_t *convert_ns)
    {
        if (channels == 1 && input_rate == OPL_SAMPLE_RATE)
            return publish_pcm(music, pcm, static_cast<unsigned>(samples));

        int16_t block[block_size];
        unsigned count = 0;
        uint64_t start = SDL_GetTicksNS();
        for (size_t i = 0; i < samples; i += channels) {
            const int current = channels == 1 ? pcm[i] :
                (static_cast<int>(pcm[i]) + pcm[i + 1]) / 2;
            if (have_previous) {
                while (position < OPL_SAMPLE_RATE) {
                    // The weighted sum stays within signed 32-bit range at
                    // this mixer rate, including opposite full-scale samples.
                    const int value = previous * static_cast<int>(OPL_SAMPLE_RATE - position)
                                    + current * static_cast<int>(position);
                    block[count++] = static_cast<int16_t>(value / static_cast<int>(OPL_SAMPLE_RATE));
                    position += input_rate;
                    if (count == block_size) {
                        *convert_ns += SDL_GetTicksNS() - start;
                        if (!publish_pcm(music, block, count)) return false;
                        count = 0;
                        start = SDL_GetTicksNS();
                    }
                }
                position -= OPL_SAMPLE_RATE;
            }
            previous = static_cast<int16_t>(current);
            have_previous = true;
        }
        *convert_ns += SDL_GetTicksNS() - start;
        return publish_pcm(music, block, count);
    }
};

int SDLCALL stream_music(void *userdata)
{
    auto &music = *static_cast<VitaMusicStream *>(userdata);
    const auto cancelled = [&music] {
        return music.cancelled.load(std::memory_order_relaxed);
    };
    const auto fail = [&music, &cancelled] {
        if (!cancelled()) {
            music.failed.store(true, std::memory_order_release);
            SDL_Log("Cosmo audio: Vita MP3 stream failed for %s", music.path);
        }
        return 0;
    };

    uint64_t start = SDL_GetTicksNS();
    size_t size = 0;
    std::unique_ptr<void, decltype(&SDL_free)> file(SDL_LoadFile(music.path, &size), SDL_free);
    const uint64_t read_ns = SDL_GetTicksNS() - start;
    music.read_us.store(static_cast<int>(read_ns / 1000));
    if (!file || cancelled()) return fail();
    const auto *data = static_cast<const uint8_t *>(file.get());
    size_t offset;
    mp3_header header;
    if (!find_first_frame(data, size, &offset, &header)) return fail();
    music.source_rate.store(static_cast<int>(header.sample_rate));
    music.source_channels.store(static_cast<int>(header.channels));

    start = SDL_GetTicksNS();
    {
        std::lock_guard<std::mutex> lock(decoder_mutex);
        if (!init_decoder_library()) return fail();
    }
    uint64_t setup_ns = SDL_GetTicksNS() - start, decode_ns = 0, convert_ns = 0;
#ifdef COSMO_VITA_AUDIO_DEBUG
    bool first_pass = true;
    bool reported_mix = false;
#endif
    PcmConverter converter{header.sample_rate, header.channels};
    const auto sink = [&](const int16_t *pcm, size_t samples) {
        if (!converter.push(music, pcm, samples, &convert_ns)) return false;
#ifdef COSMO_VITA_AUDIO_DEBUG
        if (!reported_mix) {
            const int first_mix = music.first_mix_ms.load(std::memory_order_relaxed);
            if (first_mix >= 0) {
                SDL_Log("Cosmo audio: MP3 first audio mix %d ms (%s)", first_mix, music.path);
                reported_mix = true;
            }
        }
#endif
        if (music.ready_ms.load(std::memory_order_relaxed) < 0 &&
            music.written.load(std::memory_order_relaxed) >= music.prebuffer) {
            const auto elapsed = (SDL_GetTicksNS() - music.started_ns) / SDL_NS_PER_MS;
            music.setup_us.store(static_cast<int>(setup_ns / 1000));
            music.decode_us.store(static_cast<int>(decode_ns / 1000));
            music.convert_us.store(static_cast<int>(convert_ns / 1000));
            music.ready_ms.store(static_cast<int>(elapsed), std::memory_order_relaxed);
#ifdef COSMO_VITA_AUDIO_DEBUG
            SDL_Log("Cosmo audio: MP3 prebuffer %llu ms (%s), read %llu us, setup %llu us, "
                    "decode %llu us, downmix/resample %llu us, PCM ring %u bytes allocated in %d us",
                    static_cast<unsigned long long>(elapsed), music.path,
                    static_cast<unsigned long long>(read_ns / 1000),
                    static_cast<unsigned long long>(setup_ns / 1000),
                    static_cast<unsigned long long>(decode_ns / 1000),
                    static_cast<unsigned long long>(convert_ns / 1000),
                    static_cast<unsigned>(sizeof music.pcm), music.allocation_us);
#endif
        }
        return true;
    };
    while (!cancelled()) {
        if (!decode_frames(data, size, offset, header, sink, cancelled,
                           &setup_ns, &decode_ns)) return fail();
#ifdef COSMO_VITA_AUDIO_DEBUG
        if (first_pass && !cancelled()) {
            SDL_Log("Cosmo audio: MP3 first loop work: setup %llu us, decode %llu us, "
                    "downmix/resample %llu us (excludes ring waits)",
                    static_cast<unsigned long long>(setup_ns / 1000),
                    static_cast<unsigned long long>(decode_ns / 1000),
                    static_cast<unsigned long long>(convert_ns / 1000));
            first_pass = false;
        }
#endif
    }
    return 0;
}

}  // namespace

extern "C" VitaMusicStream *vita_music_start(const char *path)
{
    SDL_PathInfo info;
    if (!path || !SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE)
        return nullptr;
    const uint64_t started = SDL_GetTicksNS();
    auto *music = new (std::nothrow) VitaMusicStream;
    if (!music) return nullptr;
    music->started_ns = started;
    music->allocation_us = static_cast<int>((SDL_GetTicksNS() - started) / 1000);
    music->path = SDL_strdup(path);
    if (music->path) music->worker = SDL_CreateThread(stream_music, "cosmo-vita-mp3", music);
    if (!music->worker) {
        vita_music_stop(music);
        return nullptr;
    }
    return music;
}

extern "C" void vita_music_stop(VitaMusicStream *music)
{
    if (!music) return;
    music->cancelled.store(true, std::memory_order_relaxed);
    if (music->worker) SDL_WaitThread(music->worker, nullptr);
    SDL_free(music->path);
    delete music;
}

extern "C" void vita_music_render(VitaMusicStream *music, int16_t *out, int frames)
{
    unsigned read = music->read.load(std::memory_order_relaxed);
    const unsigned available = music->written.load(std::memory_order_acquire) - read;
    if (!music->playing && available >= music->prebuffer) {
        music->playing = true;
        if (music->first_mix_ms.load(std::memory_order_relaxed) < 0)
            music->first_mix_ms.store(static_cast<int>((SDL_GetTicksNS() - music->started_ns) /
                                                       SDL_NS_PER_MS), std::memory_order_relaxed);
    }
    unsigned n = 0;
    if (music->playing) {
        n = std::min(available, static_cast<unsigned>(frames));
        const unsigned position = read % music->capacity;
        const unsigned first = std::min(n, music->capacity - position);
        SDL_memcpy(out, music->pcm + position, first * sizeof(*out));
        SDL_memcpy(out + first, music->pcm, (n - first) * sizeof(*out));
        read += n;
        music->read.store(read, std::memory_order_release);
        if (n < static_cast<unsigned>(frames)) {
            music->playing = false;
            music->underruns.fetch_add(1, std::memory_order_relaxed);
        }
    }
    SDL_memset(out + n, 0, (static_cast<unsigned>(frames) - n) * sizeof(*out));
}

extern "C" VitaMusicStats vita_music_stats(const VitaMusicStream *music)
{
    if (!music) return {0, -1, -1, 0, 0, 0, 0, 0, 0, 0, 0, false};
    const unsigned read = music->read.load(std::memory_order_relaxed);
    const unsigned buffered = music->written.load(std::memory_order_acquire) - read;
    return {static_cast<int>(buffered * 1000 / OPL_SAMPLE_RATE),
            music->ready_ms.load(std::memory_order_relaxed),
            music->first_mix_ms.load(std::memory_order_relaxed),
            music->underruns.load(std::memory_order_relaxed),
            music->read_us.load(), music->setup_us.load(), music->decode_us.load(),
            music->convert_us.load(), music->allocation_us, music->source_rate.load(),
            music->source_channels.load(),
            music->failed.load(std::memory_order_acquire)};
}

extern "C" bool cosmo_mp3_decode(const char *path, int16_t **samples,
                                  uint64_t *frames)
{
    if (!path || !samples || !frames) return false;
    *samples = nullptr;
    *frames = 0;

    std::lock_guard<std::mutex> lock(decoder_mutex);
    if (!init_decoder_library()) return false;

    size_t file_size = 0;
    std::unique_ptr<void, decltype(&SDL_free)> file(SDL_LoadFile(path, &file_size),
                                                    SDL_free);
    if (!file || file_size < 4) {
        SDL_Log("Cosmo audio: could not read MP3%s%s",
                path ? " " : "", path ? path : "");
        return false;
    }

    const auto *data = static_cast<const uint8_t *>(file.get());
    size_t first_frame = 0;
    mp3_header header;
    if (!find_first_frame(data, file_size, &first_frame, &header)) {
        SDL_Log("Cosmo audio: no MP3 frames found in %s", path);
        return false;
    }

    std::vector<int16_t> decoded;
    const auto collect = [&decoded](const int16_t *pcm, size_t count) {
        if (count > decoded.max_size() - decoded.size()) return false;
        try {
            decoded.insert(decoded.end(), pcm, pcm + count);
            return true;
        } catch (const std::bad_alloc &) {
            return false;
        }
    };
    if (!decode_frames(data, file_size, first_frame, header, collect,
                       [] { return false; })) {
        SDL_Log("Cosmo audio: Vita MP3 decoding failed for %s", path);
        return false;
    }

    const size_t source_frames = decoded.size() / header.channels;
    const uint64_t output_frames =
        (static_cast<uint64_t>(source_frames) * OPL_SAMPLE_RATE +
         header.sample_rate - 1) /
        header.sample_rate;
    if (!output_frames || output_frames > SIZE_MAX / sizeof(int16_t)) return false;

    int16_t *output = static_cast<int16_t *>(
        std::malloc(static_cast<size_t>(output_frames) * sizeof(int16_t)));
    if (!output) return false;

    for (uint64_t i = 0; i < output_frames; ++i) {
        const double position =
            static_cast<double>(i) * header.sample_rate / OPL_SAMPLE_RATE;
        const size_t a = static_cast<size_t>(position);
        const size_t b = a + 1 < source_frames ? a + 1 : a;
        const double fraction = position - static_cast<double>(a);
        const int32_t first = header.channels == 1
            ? decoded[a]
            : (static_cast<int32_t>(decoded[a * 2]) + decoded[a * 2 + 1]) / 2;
        const int32_t second = header.channels == 1
            ? decoded[b]
            : (static_cast<int32_t>(decoded[b * 2]) + decoded[b * 2 + 1]) / 2;
        double value = first + (second - first) * fraction;
        if (value > 32767.0) value = 32767.0;
        if (value < -32768.0) value = -32768.0;
        output[i] = static_cast<int16_t>(value);
    }

    *samples = output;
    *frames = output_frames;
    return true;
}
