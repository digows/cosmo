#include <psp2/audiodec.h>
#include <psp2/sysmodule.h>

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

#include "cosmo/opl.h"

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
    param.mp3.totalStreams = 1;

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

bool decode_frames(const uint8_t *data, size_t size, size_t offset,
                   const mp3_header &header, std::vector<int16_t> *decoded)
{
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

    while (offset + 4 <= size) {
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

        const SceInt32 result = sceAudiodecDecode(&decoder.control);
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
        if (sample_count > decoded->max_size() - decoded->size()) return false;
        try {
            decoded->insert(decoded->end(), pcm, pcm + sample_count);
        } catch (const std::bad_alloc &) {
            return false;
        }
        ++decoded_frames;
    }

    return decoded_frames > 0 && !decoded->empty();
}

}  // namespace

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
    if (!decode_frames(data, file_size, first_frame, header, &decoded)) {
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
