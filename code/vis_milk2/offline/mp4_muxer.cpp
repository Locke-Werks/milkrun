#include "mp4_muxer.h"
#include "wav_reader.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace offline {

namespace {

const LONGLONG kHnsPerSecond = 10000000LL;

// The AAC encoder built into Windows accepts these two rates only.
int NearestAacRate(int rate)
{
    return (rate == 44100) ? 44100 : 48000;
}

std::wstring Hr(const wchar_t* what, HRESULT hr)
{
    wchar_t buf[256];
    swprintf_s(buf, L"%s (0x%08X).", what, (unsigned)hr);
    return buf;
}

} // namespace

struct Mp4Muxer::Impl
{
    bool             mfStarted;
    IMFSinkWriter*   writer;
    IMFSourceReader* audioReader;
    WavReader*       wav;          // used when Media Foundation will not read the file

    DWORD videoStream;
    DWORD audioStream;
    bool  haveAudio;
    bool  writing;
    bool  finalized;

    LONGLONG frameDurationHns;
    LONGLONG audioPosHns;     // how far the audio track has been written
    LONGLONG audioEndHns;     // where the video ends, so the track is cut to match
    bool     audioDone;

    int          audioRate;
    int          audioChannels;
    std::wstring outputPath;

    Impl()
        : mfStarted(false), writer(NULL), audioReader(NULL), wav(NULL),
          videoStream(0), audioStream(0), haveAudio(false), writing(false),
          finalized(false), frameDurationHns(0), audioPosHns(0), audioEndHns(0), audioDone(false),
          audioRate(48000), audioChannels(2)
    {
    }
};

Mp4Muxer::Mp4Muxer() : m_impl(new Impl()) {}

Mp4Muxer::~Mp4Muxer()
{
    Abort();
    delete m_impl;
    m_impl = NULL;
}

bool Mp4Muxer::Init(const MuxerInitParams& params, std::wstring& error)
{
    Impl& d = *m_impl;
    d.outputPath = params.outputPath;

    if (params.outputPath.empty())
    {
        error = L"No output file given.";
        return false;
    }
    if (params.sequenceParams.empty())
    {
        // Without these the sink accepts every sample and then fails at Finalize
        // with a message that says nothing useful, so catch it here instead.
        error = L"The encoder did not supply HEVC parameter sets, so the file could not be started.";
        return false;
    }

    if (FAILED(MFStartup(MF_VERSION)))
    {
        error = L"Could not start Media Foundation.";
        return false;
    }
    d.mfStarted = true;

    d.frameDurationHns = (params.fpsNum > 0)
                       ? (kHnsPerSecond * params.fpsDen / params.fpsNum)
                       : (kHnsPerSecond / 60);

    // Cut the audio at the end of the video. Without this, Finalize drains the rest
    // of the song into a file whose picture stopped long before.
    d.audioEndHns = (params.totalFrames > 0)
                  ? params.totalFrames * d.frameDurationHns
                  : 0;   // 0 means run to the end of the song

    // Throttling exists to pace realtime capture; offline it just slows the render.
    IMFAttributes* attrs = NULL;
    if (SUCCEEDED(MFCreateAttributes(&attrs, 2)))
    {
        attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    }

    HRESULT hr = MFCreateSinkWriterFromURL(params.outputPath.c_str(), NULL, attrs, &d.writer);
    if (attrs) attrs->Release();
    if (FAILED(hr))
    {
        error = Hr(L"Could not create the output file", hr);
        Abort();
        return false;
    }

    // --- video: passthrough ------------------------------------------------
    IMFMediaType* videoType = NULL;
    if (FAILED(MFCreateMediaType(&videoType)))
    {
        error = L"Could not create the video media type.";
        Abort();
        return false;
    }

    videoType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    videoType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_HEVC);
    MFSetAttributeSize(videoType, MF_MT_FRAME_SIZE, params.width, params.height);
    MFSetAttributeRatio(videoType, MF_MT_FRAME_RATE, params.fpsNum, params.fpsDen);
    MFSetAttributeRatio(videoType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    videoType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    videoType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, FALSE);
    videoType->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER,
                       &params.sequenceParams[0],
                       (UINT32)params.sequenceParams.size());

    if (params.pq2020)
    {
        // Belt and braces alongside the VUI the encoder already wrote, so players
        // that read container metadata rather than the bitstream also get it right.
        videoType->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT2020);
        videoType->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_2084);
        videoType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT2020_10);
        videoType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        videoType->SetUINT32(MF_MT_MAX_LUMINANCE_LEVEL, (UINT32)params.peakNits);
    }

    hr = d.writer->AddStream(videoType, &d.videoStream);
    if (SUCCEEDED(hr))
    {
        // Input type identical to output type: the writer muxes and does not
        // re-encode.
        hr = d.writer->SetInputMediaType(d.videoStream, videoType, NULL);
    }
    videoType->Release();

    if (FAILED(hr))
    {
        error = Hr(L"Windows would not accept an HEVC track for this file", hr);
        Abort();
        return false;
    }

    // --- audio: decode the song, let the sink re-encode to AAC --------------
    if (!params.audioPath.empty())
    {
        // Same reason as the visualizer's audio path: Media Foundation stops at
        // 32-bit float PCM, so an ordinary 64-bit float DAW export has to be read
        // directly or the track cannot be muxed at all.
        if (WavReader::IsRiffWave(params.audioPath))
        {
            WavReader* w = new WavReader();
            std::wstring wavError;
            if (w->Open(params.audioPath, wavError))
            {
                d.wav = w;
                d.audioRate     = WavReader::kSampleRate;
                d.audioChannels = 2;
                if (params.startSeconds > 0.0)
                {
                    std::wstring skipError;
                    w->Skip((long long)(params.startSeconds * WavReader::kSampleRate + 0.5), skipError);
                }
            }
            else
            {
                delete w;
            }
        }

        if (!d.wav &&
            SUCCEEDED(MFCreateSourceReaderFromURL(params.audioPath.c_str(), NULL, &d.audioReader)))
        {
            d.audioReader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
            d.audioReader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

            // Follow the source rate where the AAC encoder allows it, so a 44100
            // source is not needlessly resampled.
            int nativeRate = 48000;
            IMFMediaType* native = NULL;
            if (SUCCEEDED(d.audioReader->GetNativeMediaType(
                    (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &native)) && native)
            {
                UINT32 r = 0;
                if (SUCCEEDED(native->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &r)) && r)
                    nativeRate = (int)r;
                native->Release();
            }
            d.audioRate     = NearestAacRate(nativeRate);
            d.audioChannels = 2;

            if (params.startSeconds > 0.0)
            {
                PROPVARIANT pos;
                PropVariantInit(&pos);
                pos.vt = VT_I8;
                pos.hVal.QuadPart = (LONGLONG)(params.startSeconds * kHnsPerSecond);
                d.audioReader->SetCurrentPosition(GUID_NULL, pos);
                PropVariantClear(&pos);
            }
        }

        {
            IMFMediaType* pcm = NULL;
            if (SUCCEEDED(MFCreateMediaType(&pcm)))
            {
                pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, d.audioRate);
                pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, d.audioChannels);
                pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                pcm->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, d.audioChannels * 2);
                pcm->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, d.audioRate * d.audioChannels * 2);
                pcm->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

                HRESULT hrPcm = d.audioReader
                    ? d.audioReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, pcm)
                    : S_OK;

                if (SUCCEEDED(hrPcm))
                {
                    IMFMediaType* aac = NULL;
                    if (SUCCEEDED(MFCreateMediaType(&aac)))
                    {
                        aac->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                        aac->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
                        aac->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, d.audioRate);
                        aac->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, d.audioChannels);
                        aac->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                        aac->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                       params.audioBitrateKbps * 1000 / 8);

                        if (SUCCEEDED(d.writer->AddStream(aac, &d.audioStream)) &&
                            SUCCEEDED(d.writer->SetInputMediaType(d.audioStream, pcm, NULL)))
                        {
                            d.haveAudio = true;
                        }
                        aac->Release();
                    }
                }
                pcm->Release();
            }
        }

        if (!d.haveAudio)
        {
            // The visuals are still correct without it, but the caller asked for a
            // file with sound, so this is a failure rather than a quiet omission.
            error = L"Could not add the song as an audio track. Its format may not be supported.";
            Abort();
            return false;
        }
    }

    hr = d.writer->BeginWriting();
    if (FAILED(hr))
    {
        error = Hr(L"Could not begin writing the output file", hr);
        Abort();
        return false;
    }
    d.writing = true;
    return true;
}

bool Mp4Muxer::PumpAudio(LONGLONG untilHns, std::wstring& error)
{
    Impl& d = *m_impl;
    if (!d.haveAudio || d.audioDone)
        return true;

    // Direct WAV path: build the samples ourselves rather than pulling them from a
    // source reader that cannot open this file.
    if (d.wav)
    {
        const size_t kChunk = 4096;
        std::vector<float> block(kChunk * 2);
        std::vector<short> pcm(kChunk * 2);

        while (untilHns < 0 || d.audioPosHns < untilHns)
        {
            size_t got = 0;
            std::wstring wavError;
            if (!d.wav->ReadSamples(&block[0], kChunk, got, wavError))
            {
                error = wavError;
                return false;
            }
            if (got == 0)
            {
                d.audioDone = true;
                return true;
            }

            for (size_t i = 0; i < got * 2; i++)
            {
                float v = block[i];
                if (v >  1.0f) v =  1.0f;
                if (v < -1.0f) v = -1.0f;
                pcm[i] = (short)(v * 32767.0f);
            }

            const DWORD bytes = (DWORD)(got * 2 * sizeof(short));
            IMFMediaBuffer* buffer = NULL;
            if (FAILED(MFCreateMemoryBuffer(bytes, &buffer)))
            {
                error = L"Out of memory while writing the audio track.";
                return false;
            }

            BYTE* dst = NULL;
            DWORD maxLen = 0;
            if (FAILED(buffer->Lock(&dst, &maxLen, NULL)))
            {
                buffer->Release();
                error = L"Could not stage the audio track for writing.";
                return false;
            }
            memcpy(dst, &pcm[0], bytes);
            buffer->Unlock();
            buffer->SetCurrentLength(bytes);

            IMFSample* sample = NULL;
            if (FAILED(MFCreateSample(&sample)))
            {
                buffer->Release();
                error = L"Could not create an audio sample.";
                return false;
            }
            sample->AddBuffer(buffer);
            buffer->Release();

            const LONGLONG dur = (LONGLONG)got * kHnsPerSecond / d.audioRate;
            sample->SetSampleTime(d.audioPosHns);
            sample->SetSampleDuration(dur);

            const HRESULT hrW = d.writer->WriteSample(d.audioStream, sample);
            sample->Release();
            if (FAILED(hrW))
            {
                error = Hr(L"Failed while writing the audio track", hrW);
                return false;
            }

            d.audioPosHns += dur;
        }
        return true;
    }

    while (untilHns < 0 || d.audioPosHns < untilHns)
    {
        DWORD flags = 0;
        IMFSample* sample = NULL;
        LONGLONG ts = 0;

        HRESULT hr = d.audioReader->ReadSample(
            MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, NULL, &flags, &ts, &sample);

        if (FAILED(hr))
        {
            if (sample) sample->Release();
            error = Hr(L"Failed while reading the song for the audio track", hr);
            return false;
        }

        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
        {
            d.audioDone = true;
            if (sample) sample->Release();
            return true;
        }

        if (!sample)
            continue;

        LONGLONG dur = 0;
        sample->GetSampleDuration(&dur);
        sample->SetSampleTime(d.audioPosHns);

        hr = d.writer->WriteSample(d.audioStream, sample);
        sample->Release();

        if (FAILED(hr))
        {
            error = Hr(L"Failed while writing the audio track", hr);
            return false;
        }

        d.audioPosHns += (dur > 0) ? dur : 0;
        if (dur <= 0)
            break;   // no duration to advance on; avoid spinning
    }
    return true;
}

bool Mp4Muxer::WriteVideoPacket(const EncodedPacket& pkt, std::wstring& error)
{
    Impl& d = *m_impl;
    if (!d.writing)
    {
        error = L"The output file is not open.";
        return false;
    }

    IMFMediaBuffer* buffer = NULL;
    if (FAILED(MFCreateMemoryBuffer((DWORD)pkt.size, &buffer)))
    {
        error = L"Out of memory while writing a video frame.";
        return false;
    }

    BYTE* dst = NULL;
    DWORD maxLen = 0;
    if (FAILED(buffer->Lock(&dst, &maxLen, NULL)))
    {
        buffer->Release();
        error = L"Could not stage a video frame for writing.";
        return false;
    }
    memcpy(dst, pkt.data, pkt.size);
    buffer->Unlock();
    buffer->SetCurrentLength((DWORD)pkt.size);

    IMFSample* sample = NULL;
    if (FAILED(MFCreateSample(&sample)))
    {
        buffer->Release();
        error = L"Could not create a video sample.";
        return false;
    }
    sample->AddBuffer(buffer);
    buffer->Release();

    const LONGLONG pts = pkt.frameIndex * d.frameDurationHns;
    sample->SetSampleTime(pts);
    sample->SetSampleDuration(d.frameDurationHns);
    if (pkt.keyframe)
        sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);

    const HRESULT hr = d.writer->WriteSample(d.videoStream, sample);
    sample->Release();

    if (FAILED(hr))
    {
        error = Hr(L"Failed while writing a video frame", hr);
        return false;
    }

    // Keep the audio track roughly level with the video so the sink never has to
    // buffer a large one-sided backlog.
    return PumpAudio(pts + d.frameDurationHns, error);
}

bool Mp4Muxer::Finalize(std::wstring& error)
{
    Impl& d = *m_impl;
    if (!d.writing)
    {
        error = L"The output file is not open.";
        return false;
    }

    // Not -1: that would drain the rest of the song past the end of the picture.
    if (!PumpAudio(d.audioEndHns > 0 ? d.audioEndHns : -1, error))
        return false;

    const HRESULT hr = d.writer->Finalize();
    d.writing = false;

    if (FAILED(hr))
    {
        error = Hr(L"Could not finish writing the output file", hr);
        return false;
    }

    d.finalized = true;
    return true;
}

void Mp4Muxer::Abort()
{
    Impl& d = *m_impl;

    if (d.writer)  { d.writer->Release();  d.writer = NULL; }
    if (d.audioReader) { d.audioReader->Release(); d.audioReader = NULL; }
    if (d.wav) { delete d.wav; d.wav = NULL; }

    // Never leave a partial file behind for a render that did not complete.
    if (!d.finalized && !d.outputPath.empty())
        DeleteFileW(d.outputPath.c_str());

    d.writing = false;

    if (d.mfStarted) { MFShutdown(); d.mfStarted = false; }
}

} // namespace offline
