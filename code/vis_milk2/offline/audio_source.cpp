#include "audio_source.h"
#include "wav_reader.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>
#include <math.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "propsys.lib")

namespace offline {

namespace {

// Ring capacity. Only the most recent window is ever read, so this just needs to
// comfortably exceed one window plus one decoded packet.
const int kRingSamples = 1 << 16;   // 65536 stereo samples, ~1.5s at 44100

// Same conversion the live loopback path uses in audio/audiobuf.cpp, so a rendered
// frame reacts to a given sample exactly as the live visualizer would.
inline signed char FloatToInt8(float f)
{
    if (f >= 1.0f)  return  127;
    if (f < -1.0f)  return -128;
    return (signed char)(f * 128);
}

// When negotiation fails, the useful thing to report is what the file actually is,
// not that it failed. Media Foundation's PCM support tops out at 32-bit float, so
// exotic WAV bit depths land here.
std::wstring DescribeUndecodable(IMFSourceReader* reader, HRESULT hr)
{
    wchar_t buf[512];
    IMFMediaType* native = NULL;

    if (SUCCEEDED(reader->GetNativeMediaType(
            (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &native)) && native)
    {
        GUID sub = GUID_NULL;
        UINT32 rate = 0, ch = 0, bits = 0;
        native->GetGUID(MF_MT_SUBTYPE, &sub);
        native->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
        native->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
        native->Release();

        const wchar_t* kind = L"PCM";
        if (sub == MFAudioFormat_Float) kind = L"float PCM";

        swprintf_s(buf,
            L"Windows cannot decode this audio file (%u-bit %s, %u Hz, %u ch). "
            L"Media Foundation handles up to 32-bit float PCM; convert the file "
            L"first, for example to 24-bit or 32-bit float WAV or to FLAC.",
            bits, kind, rate, ch);
        return buf;
    }

    swprintf_s(buf, L"Windows cannot decode this audio file (error 0x%08X).", (unsigned)hr);
    return buf;
}
} // namespace

AudioSource::AudioSource()
    : m_reader(NULL), m_mfStarted(false), m_durationSeconds(0.0),
      m_totalSamples(0), m_ringFirst(0), m_ringCount(0), m_eof(false), m_sourceIsFloat(true),
      m_wav(NULL)
{
}

AudioSource::~AudioSource()
{
    Close();
}

void AudioSource::Close()
{
    if (m_wav) { delete m_wav; m_wav = NULL; }
    if (m_reader) { m_reader->Release(); m_reader = NULL; }
    if (m_mfStarted) { MFShutdown(); m_mfStarted = false; }
    m_ring.clear();
    m_ringFirst = m_ringCount = m_totalSamples = 0;
    m_durationSeconds = 0.0;
    m_eof = false;
}

bool AudioSource::Open(const std::wstring& path, std::wstring& error)
{
    Close();

    if (path.empty())
    {
        error = L"No audio file given.";
        return false;
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        error = L"Audio file not found.";
        return false;
    }

    // Media Foundation's PCM support stops at 32-bit float, so a 64-bit float WAV,
    // which is an ordinary DAW export, never negotiates a type. Read WAVs directly.
    // A compressed one (ADPCM, mu-law) fails here and falls through on purpose,
    // because Media Foundation does have decoders for those.
    if (WavReader::IsRiffWave(path))
    {
        WavReader* wav = new WavReader();
        std::wstring wavError;
        if (wav->Open(path, wavError))
        {
            m_wav = wav;
            m_durationSeconds = wav->DurationSeconds();
            m_totalSamples = wav->TotalSamples();
            m_ring.assign((size_t)kRingSamples * 2, 0.0f);
            m_ringFirst = 0;
            m_ringCount = 0;
            m_eof = false;
            return true;
        }
        delete wav;
    }

    if (FAILED(MFStartup(MF_VERSION)))
    {
        error = L"Could not start Media Foundation.";
        return false;
    }
    m_mfStarted = true;

    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), NULL, &m_reader)))
    {
        error = L"Could not open the audio file. The format may not be supported.";
        Close();
        return false;
    }

    m_reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    m_reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

    // Ask for 44100 Hz stereo float; Media Foundation inserts a resampler and a
    // channel matrixer as needed. Not every source negotiates float, so fall back
    // to 16-bit PCM before giving up.
    m_sourceIsFloat = true;
    HRESULT hrType = E_FAIL;

    for (int attempt = 0; attempt < 2 && FAILED(hrType); attempt++)
    {
        const bool wantFloat = (attempt == 0);
        const UINT32 bits    = wantFloat ? 32 : 16;
        const UINT32 align   = bits / 8 * 2;

        IMFMediaType* type = NULL;
        if (FAILED(MFCreateMediaType(&type)))
        {
            error = L"Could not create the audio output type.";
            Close();
            return false;
        }

        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        type->SetGUID(MF_MT_SUBTYPE, wantFloat ? MFAudioFormat_Float : MFAudioFormat_PCM);
        type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kSampleRate);
        type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, bits);
        type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, align);
        type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kSampleRate * align);
        type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

        hrType = m_reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, type);
        type->Release();

        if (SUCCEEDED(hrType))
            m_sourceIsFloat = wantFloat;
    }

    if (FAILED(hrType))
    {
        error = DescribeUndecodable(m_reader, hrType);
        Close();
        return false;
    }

    PROPVARIANT var;
    PropVariantInit(&var);
    if (SUCCEEDED(m_reader->GetPresentationAttribute(
            (DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var)))
    {
        LONGLONG hns = 0;
        if (SUCCEEDED(PropVariantToInt64(var, &hns)))
            m_durationSeconds = (double)hns / 10000000.0;   // 100ns units
    }
    PropVariantClear(&var);

    if (m_durationSeconds <= 0.0)
    {
        error = L"Could not determine the length of the audio file.";
        Close();
        return false;
    }

    m_totalSamples = (long long)(m_durationSeconds * kSampleRate + 0.5);
    m_ring.assign((size_t)kRingSamples * 2, 0.0f);
    m_ringFirst = 0;
    m_ringCount = 0;
    m_eof = false;
    return true;
}

long long AudioSource::FrameCount(int fpsNum, int fpsDen) const
{
    if (fpsNum <= 0 || fpsDen <= 0)
        return 0;

    // Round up so the last partial frame interval is still covered: the video runs
    // for at least as long as the song and never longer than one frame past it.
    const double frames = m_durationSeconds * (double)fpsNum / (double)fpsDen;
    long long n = (long long)ceil(frames - 1e-9);
    return (n > 0) ? n : 1;
}

bool AudioSource::EnsureBuffered(long long endSample)
{
    // The WAV reader hands back the same 44100 Hz stereo float the Media Foundation
    // path negotiates, so it feeds the same ring.
    if (m_wav)
    {
        while (!m_eof && (m_ringFirst + m_ringCount) < endSample)
        {
            float block[512 * 2];
            size_t got = 0;
            std::wstring wavError;

            if (!m_wav->ReadSamples(block, 512, got, wavError))
                return false;

            if (got == 0)
            {
                m_eof = true;
                break;
            }

            for (size_t i = 0; i < got; i++)
            {
                const long long abs = m_ringFirst + m_ringCount;
                const size_t slot = (size_t)(abs % kRingSamples) * 2;
                m_ring[slot + 0] = block[i * 2 + 0];
                m_ring[slot + 1] = block[i * 2 + 1];
                m_ringCount++;

                if (m_ringCount > kRingSamples)
                {
                    m_ringFirst += (m_ringCount - kRingSamples);
                    m_ringCount = kRingSamples;
                }
            }
        }
        return true;
    }

    while (!m_eof && (m_ringFirst + m_ringCount) < endSample)
    {
        DWORD streamFlags = 0;
        IMFSample* sample = NULL;

        const HRESULT hr = m_reader->ReadSample(
            MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, NULL, &streamFlags, NULL, &sample);

        if (FAILED(hr))
        {
            if (sample) sample->Release();
            return false;
        }

        if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM)
        {
            m_eof = true;
            if (sample) sample->Release();
            break;
        }

        if (!sample)
            continue;   // a gap or a format change; keep pulling

        IMFMediaBuffer* buffer = NULL;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && buffer)
        {
            BYTE* data = NULL;
            DWORD length = 0;
            if (SUCCEEDED(buffer->Lock(&data, NULL, &length)))
            {
                const size_t stride = m_sourceIsFloat ? sizeof(float) * 2 : sizeof(short) * 2;
                const long long got = length / stride;

                for (long long i = 0; i < got; i++)
                {
                    float l, r;
                    if (m_sourceIsFloat)
                    {
                        const float* src = (const float*)data;
                        l = src[i * 2 + 0];
                        r = src[i * 2 + 1];
                    }
                    else
                    {
                        const short* src = (const short*)data;
                        l = src[i * 2 + 0] / 32768.0f;
                        r = src[i * 2 + 1] / 32768.0f;
                    }

                    const long long abs = m_ringFirst + m_ringCount;
                    const size_t slot = (size_t)(abs % kRingSamples) * 2;
                    m_ring[slot + 0] = l;
                    m_ring[slot + 1] = r;
                    m_ringCount++;

                    // Once full, the window slides: drop the oldest sample.
                    if (m_ringCount > kRingSamples)
                    {
                        m_ringFirst += (m_ringCount - kRingSamples);
                        m_ringCount = kRingSamples;
                    }
                }
                buffer->Unlock();
            }
            buffer->Release();
        }
        sample->Release();
    }
    return true;
}

bool AudioSource::FillFrameWindow(long long frameIndex, int fpsNum, int fpsDen,
                                  unsigned char* outL, unsigned char* outR)
{
    if ((!m_reader && !m_wav) || !outL || !outR)
        return false;

    // Start of this frame's display interval, in samples.
    const long long start =
        (long long)(((double)frameIndex * (double)fpsDen * (double)kSampleRate)
                    / (double)fpsNum + 0.5);
    const long long end = start + kWindowSamples;

    if (!EnsureBuffered(end))
        return false;

    for (int i = 0; i < kWindowSamples; i++)
    {
        const long long abs = start + i;
        float l = 0.0f, r = 0.0f;

        // Past the end of the song, or (only possible if a caller rewinds) before
        // what the ring still holds: silence.
        if (abs >= m_ringFirst && abs < m_ringFirst + m_ringCount)
        {
            const size_t slot = (size_t)(abs % kRingSamples) * 2;
            l = m_ring[slot + 0];
            r = m_ring[slot + 1];
        }

        outL[i] = (unsigned char)FloatToInt8(l);
        outR[i] = (unsigned char)FloatToInt8(r);
    }
    return true;
}

} // namespace offline
