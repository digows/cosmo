/* Decode an MP3 through the Windows Media Foundation audio pipeline. */
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

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

    int wide_length = MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, nullptr, 0);
    if (!wide_length) return false;
    std::vector<wchar_t> wide_path;
    std::vector<int16_t> decoded;
    try {
        wide_path.resize((size_t)wide_length);
        if (!MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1,
                                 wide_path.data(), wide_length)) return false;
    } catch (const std::bad_alloc &) {
        return false;
    }

    HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool uninitialize_com = SUCCEEDED(com_result);
    if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) return false;

    HRESULT result = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    IMFSourceReader *reader = nullptr;
    IMFMediaType *media_type = nullptr;
    bool mf_started = SUCCEEDED(result);
    bool ok = false;

    if (SUCCEEDED(result))
        result = MFCreateSourceReaderFromURL(wide_path.data(), nullptr, &reader);
    if (SUCCEEDED(result)) result = MFCreateMediaType(&media_type);
    if (SUCCEEDED(result)) result = media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(result)) result = media_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    if (SUCCEEDED(result)) result = media_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
    if (SUCCEEDED(result)) result = media_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
                                                           OPL_SAMPLE_RATE);
    if (SUCCEEDED(result)) result = media_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(result)) result = media_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2);
    if (SUCCEEDED(result)) result = media_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                                           OPL_SAMPLE_RATE * 2);
    if (SUCCEEDED(result)) result = reader->SetCurrentMediaType(
        MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, media_type);

    while (SUCCEEDED(result)) {
        DWORD flags = 0;
        IMFSample *sample = nullptr;
        result = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
                                    nullptr, &flags, nullptr, &sample);
        if (FAILED(result)) {
            if (sample) sample->Release();
            break;
        }
        if (sample) {
            IMFMediaBuffer *buffer = nullptr;
            result = sample->ConvertToContiguousBuffer(&buffer);
            if (SUCCEEDED(result)) {
                BYTE *data = nullptr;
                DWORD length = 0;
                result = buffer->Lock(&data, nullptr, &length);
                if (SUCCEEDED(result)) {
                    try {
                        const int16_t *start = reinterpret_cast<const int16_t *>(data);
                        decoded.insert(decoded.end(), start, start + length / sizeof(int16_t));
                    } catch (const std::bad_alloc &) {
                        result = E_OUTOFMEMORY;
                    }
                    buffer->Unlock();
                }
                buffer->Release();
            }
            sample->Release();
            if (FAILED(result)) break;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }

    if (reader) reader->Release();
    if (media_type) media_type->Release();
    if (mf_started) MFShutdown();
    if (uninitialize_com) CoUninitialize();

    if (SUCCEEDED(result) && !decoded.empty()) {
        int16_t *copy = static_cast<int16_t *>(malloc(decoded.size() * sizeof(int16_t)));
        if (!copy) return false;
        memcpy(copy, decoded.data(), decoded.size() * sizeof(int16_t));
        *samples = copy;
        *frames = decoded.size();
        ok = true;
    }
    return ok;
}
