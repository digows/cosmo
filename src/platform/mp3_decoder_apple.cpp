/* Decode an MP3 through Apple's AudioToolbox ExtAudioFile pipeline. */
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include "cosmo/opl.h"

extern "C" bool cosmo_mp3_decode(const char *utf8_path, int16_t **samples,
                                  uint64_t *frames)
{
    if (!utf8_path || !samples || !frames) return false;
    *samples = nullptr;
    *frames = 0;

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8 *>(utf8_path),
        static_cast<CFIndex>(strlen(utf8_path)),
        false);
    if (!url) return false;

    ExtAudioFileRef audio_file = nullptr;
    OSStatus status = ExtAudioFileOpenURL(url, &audio_file);
    CFRelease(url);
    if (status != noErr || !audio_file) return false;

    AudioStreamBasicDescription file_format = {};
    UInt32 prop_size = static_cast<UInt32>(sizeof file_format);
    status = ExtAudioFileGetProperty(
        audio_file, kExtAudioFileProperty_FileDataFormat,
        &prop_size, &file_format);
    UInt32 channels = file_format.mChannelsPerFrame;
    if (status != noErr || channels < 1 || channels > 8) {
        ExtAudioFileDispose(audio_file);
        return false;
    }

    AudioStreamBasicDescription format = {};
    format.mSampleRate = static_cast<Float64>(OPL_SAMPLE_RATE);
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    format.mBitsPerChannel = 16;
    format.mChannelsPerFrame = channels;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = static_cast<UInt32>(sizeof(int16_t)) * channels;
    format.mBytesPerPacket = format.mBytesPerFrame;

    status = ExtAudioFileSetProperty(
        audio_file, kExtAudioFileProperty_ClientDataFormat,
        static_cast<UInt32>(sizeof format), &format);

    std::vector<int16_t> decoded;
    constexpr UInt32 chunk_frames = 4096;
    std::vector<int16_t> chunk;
    bool alloc_failed = false;

    if (status == noErr) {
        try {
            chunk.resize(static_cast<size_t>(chunk_frames) * channels);
        } catch (const std::bad_alloc &) {
            alloc_failed = true;
        }
    }

    while (status == noErr && !alloc_failed) {
        AudioBufferList buffer_list = {};
        buffer_list.mNumberBuffers = 1;
        buffer_list.mBuffers[0].mNumberChannels = channels;
        buffer_list.mBuffers[0].mDataByteSize =
            static_cast<UInt32>(chunk.size() * sizeof(int16_t));
        buffer_list.mBuffers[0].mData = chunk.data();

        UInt32 count = chunk_frames;
        status = ExtAudioFileRead(audio_file, &count, &buffer_list);
        if (status != noErr || count == 0) break;

        try {
            if (channels == 1) {
                decoded.insert(decoded.end(), chunk.data(), chunk.data() + count);
            } else {
                size_t base = decoded.size();
                decoded.resize(base + count);
                for (UInt32 f = 0; f < count; ++f) {
                    int32_t sum = 0;
                    const int16_t *frame =
                        chunk.data() + static_cast<size_t>(f) * channels;
                    for (UInt32 ch = 0; ch < channels; ++ch) sum += frame[ch];
                    decoded[base + f] =
                        static_cast<int16_t>(sum / static_cast<int32_t>(channels));
                }
            }
        } catch (const std::bad_alloc &) {
            alloc_failed = true;
            break;
        }
    }

    ExtAudioFileDispose(audio_file);

    if (status != noErr || alloc_failed || decoded.empty()) return false;

    int16_t *copy = static_cast<int16_t *>(malloc(decoded.size() * sizeof(int16_t)));
    if (!copy) return false;
    memcpy(copy, decoded.data(), decoded.size() * sizeof(int16_t));
    *samples = copy;
    *frames = decoded.size();
    return true;
}
