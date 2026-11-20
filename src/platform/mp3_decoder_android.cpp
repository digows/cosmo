/* Decode MP3 assets through Android's built-in MediaExtractor and MediaCodec. */
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "cosmo/opl.h"

static bool decode_android_mp3(const char *path, int16_t **samples,
                               uint64_t *frames)
{
    if (!path || !samples || !frames) return false;
    *samples = nullptr;
    *frames = 0;

    AMediaExtractor *extractor = AMediaExtractor_new();
    AMediaCodec *codec = nullptr;
    AMediaFormat *track_format = nullptr;
    int fd = -1;
    std::vector<int16_t> decoded;
    int32_t sample_rate = OPL_SAMPLE_RATE;
    int32_t channels = 1;
    bool started = false;
    bool input_done = false;
    bool output_done = false;
    bool ok = false;

    if (!extractor) goto done;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) goto done;
    {
        struct stat file_info{};
        if (fstat(fd, &file_info) != 0 || file_info.st_size <= 0 ||
            AMediaExtractor_setDataSourceFd(extractor, fd, 0, file_info.st_size) != AMEDIA_OK)
            goto done;
    }

    for (size_t i = 0; i < AMediaExtractor_getTrackCount(extractor); ++i) {
        AMediaFormat *format = AMediaExtractor_getTrackFormat(extractor, i);
        const char *mime = nullptr;
        bool is_mp3 = format && AMediaFormat_getString(
            format, AMEDIAFORMAT_KEY_MIME, &mime) && mime &&
            std::strcmp(mime, "audio/mpeg") == 0;
        if (is_mp3) {
            track_format = format;
            if (AMediaExtractor_selectTrack(extractor, i) != AMEDIA_OK) goto done;
            break;
        }
        if (format) AMediaFormat_delete(format);
    }
    if (!track_format) goto done;

    {
        const char *mime = nullptr;
        if (!AMediaFormat_getString(track_format, AMEDIAFORMAT_KEY_MIME, &mime) ||
            !mime) goto done;
        codec = AMediaCodec_createDecoderByType(mime);
    }
    if (!codec || AMediaCodec_configure(codec, track_format, nullptr, nullptr, 0) != AMEDIA_OK ||
        AMediaCodec_start(codec) != AMEDIA_OK) goto done;
    started = true;

    while (!output_done) {
        if (!input_done) {
            ssize_t input_index = AMediaCodec_dequeueInputBuffer(codec, 10000);
            if (input_index >= 0) {
                size_t capacity = 0;
                uint8_t *input = AMediaCodec_getInputBuffer(
                    codec, static_cast<size_t>(input_index), &capacity);
                if (!input) goto done;

                ssize_t size = AMediaExtractor_readSampleData(extractor, input, capacity);
                if (size < 0) {
                    if (AMediaCodec_queueInputBuffer(codec,
                            static_cast<size_t>(input_index), 0, 0, 0,
                            AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != AMEDIA_OK) goto done;
                    input_done = true;
                } else {
                    int64_t presentation_time = AMediaExtractor_getSampleTime(extractor);
                    if (presentation_time < 0) presentation_time = 0;
                    if (AMediaCodec_queueInputBuffer(codec,
                            static_cast<size_t>(input_index), 0,
                            static_cast<size_t>(size),
                            static_cast<uint64_t>(presentation_time), 0) != AMEDIA_OK) goto done;
                    AMediaExtractor_advance(extractor);
                }
            }
        }

        {
            AMediaCodecBufferInfo info{};
            ssize_t output_index = AMediaCodec_dequeueOutputBuffer(codec, &info, 10000);
            if (output_index >= 0) {
                size_t ignored_capacity = 0;
                uint8_t *output = AMediaCodec_getOutputBuffer(
                    codec, static_cast<size_t>(output_index), &ignored_capacity);
                if (info.size < 0 || (info.size & 1) != 0 || (info.size > 0 && !output)) {
                    AMediaCodec_releaseOutputBuffer(codec,
                        static_cast<size_t>(output_index), false);
                    goto done;
                }
                if (info.size > 0) {
                    size_t count = static_cast<size_t>(info.size) / sizeof(int16_t);
                    const int16_t *pcm = reinterpret_cast<const int16_t *>(output);
                    decoded.insert(decoded.end(), pcm, pcm + count);
                }
                bool end_of_stream =
                    (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
                AMediaCodec_releaseOutputBuffer(codec,
                    static_cast<size_t>(output_index), false);
                if (end_of_stream) output_done = true;
            } else if (output_index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                AMediaFormat *format = AMediaCodec_getOutputFormat(codec);
                if (format) {
                    int32_t value = 0;
                    if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &value) &&
                        value > 0) sample_rate = value;
                    if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &value) &&
                        value > 0) channels = value;
                    AMediaFormat_delete(format);
                }
            }
        }
    }

    if (decoded.empty() || sample_rate <= 0 || channels <= 0 || channels > 8) goto done;
    {
        size_t source_frames = decoded.size() / static_cast<size_t>(channels);
        if (!source_frames || source_frames > SIZE_MAX / sizeof(int16_t)) goto done;
        uint64_t output_frames =
            (static_cast<uint64_t>(source_frames) * OPL_SAMPLE_RATE + sample_rate - 1) /
            sample_rate;
        if (!output_frames || output_frames > SIZE_MAX / sizeof(int16_t)) goto done;
        std::vector<int16_t> mono(static_cast<size_t>(output_frames));

        for (uint64_t i = 0; i < output_frames; ++i) {
            double position = static_cast<double>(i) * sample_rate / OPL_SAMPLE_RATE;
            size_t a = std::min(static_cast<size_t>(position), source_frames - 1);
            size_t b = std::min(a + 1, source_frames - 1);
            double fraction = position - static_cast<double>(a);
            int32_t left = 0, right = 0;
            for (int32_t channel = 0; channel < channels; ++channel) {
                left += decoded[a * static_cast<size_t>(channels) + channel];
                right += decoded[b * static_cast<size_t>(channels) + channel];
            }
            double a_mono = static_cast<double>(left) / channels;
            double b_mono = static_cast<double>(right) / channels;
            double value = a_mono + (b_mono - a_mono) * fraction;
            value = std::max(-32768.0, std::min(32767.0, value));
            mono[static_cast<size_t>(i)] = static_cast<int16_t>(value);
        }

        int16_t *result = static_cast<int16_t *>(
            std::malloc(static_cast<size_t>(output_frames) * sizeof(int16_t)));
        if (!result) goto done;
        std::memcpy(result, mono.data(), static_cast<size_t>(output_frames) * sizeof(int16_t));
        *samples = result;
        *frames = output_frames;
        ok = true;
    }

done:
    if (started) AMediaCodec_stop(codec);
    if (codec) AMediaCodec_delete(codec);
    if (track_format) AMediaFormat_delete(track_format);
    if (extractor) AMediaExtractor_delete(extractor);
    if (fd >= 0) close(fd);
    return ok;
}

extern "C" bool cosmo_mp3_decode(const char *path, int16_t **samples,
                                  uint64_t *frames)
{
    if (!samples || !frames) return false;
    *samples = nullptr;
    *frames = 0;
    try {
        return decode_android_mp3(path, samples, frames);
    } catch (const std::bad_alloc &) {
        return false;
    }
}
