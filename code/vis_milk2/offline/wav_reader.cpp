#include "wav_reader.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace offline {

namespace {

// Format tags, spelled out here rather than taken from mmreg.h so this file does
// not care which Windows headers the rest of the translation unit pulled in.
const unsigned int kTagPcm        = 0x0001;
const unsigned int kTagIeeeFloat  = 0x0003;
const unsigned int kTagExtensible = 0xFFFE;

// Every KSDATAFORMAT_SUBTYPE_* GUID is the plain format tag followed by these
// twelve bytes, so the real format of an extensible file is the first field.
const unsigned char kSubFormatTail[12] =
{
    0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71
};

// Half-width of the resampling filter, in zero crossings, and how finely the
// prototype is tabulated. 32 crossings with a Kaiser beta of 8.6 puts the
// stopband near -90 dB and keeps the transition band tight enough that almost
// nothing folds back over 22050 Hz at any rate a DAW exports.
const int    kZeroCrossings = 32;
const int    kFilterPhases  = 128;
const double kKaiserBeta    = 8.6;

// Cutoff as a fraction of the lower Nyquist. Pulling it slightly below puts the
// whole transition band under the fold point instead of straddling it.
const double kRolloff = 0.95;

const double kPi = 3.14159265358979323846;

// Frames of slack either side of the live filter window. The window only has to
// be compacted once this much has gone dead, rather than on every sample.
const size_t kTrimSlack = 4096;

const size_t kRawBytes = 1 << 16;

inline unsigned int Le16(const unsigned char* p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

inline unsigned int Le32(const unsigned char* p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

inline unsigned long long Le64(const unsigned char* p)
{
    return (unsigned long long)Le32(p) | ((unsigned long long)Le32(p + 4) << 32);
}

long long Gcd(long long a, long long b)
{
    while (b != 0)
    {
        const long long t = a % b;
        a = b;
        b = t;
    }
    return a;
}

// Modified Bessel function of the first kind, order zero. Only needed to build
// the Kaiser window once per file.
double BesselI0(double x)
{
    double sum  = 1.0;
    double term = 1.0;
    const double q = x * x * 0.25;

    for (int k = 1; k < 64; k++)
    {
        term *= q / ((double)k * (double)k);
        sum  += term;
        if (term < sum * 1e-17)
            break;
    }
    return sum;
}

// One sample of one channel, from its container. Sub-container widths (20-bit in
// 24, 24-bit in 32) are left justified by the spec, so full scale is the same.
inline float DecodeSample(const unsigned char* p, int bits, bool isFloat)
{
    if (isFloat)
    {
        if (bits == 64)
        {
            double d;
            memcpy(&d, p, sizeof(d));
            return (float)d;
        }
        float f;
        memcpy(&f, p, sizeof(f));
        return f;
    }

    switch (bits)
    {
    case 8:
        // 8-bit PCM is the one unsigned case in the format.
        return ((int)p[0] - 128) * (1.0f / 128.0f);

    case 16:
    {
        short v;
        memcpy(&v, p, sizeof(v));
        return v * (1.0f / 32768.0f);
    }

    case 24:
    {
        // Sitting the three bytes at the top of the word sign extends them for us.
        const int v = (int)(((unsigned int)p[2] << 24) |
                            ((unsigned int)p[1] << 16) |
                            ((unsigned int)p[0] << 8));
        return v * (1.0f / 2147483648.0f);
    }

    case 32:
    {
        int v;
        memcpy(&v, p, sizeof(v));
        return v * (1.0f / 2147483648.0f);
    }
    }
    return 0.0f;
}

// Reads the prototype filter at 'x' zero crossings from the centre, interpolating
// between table entries. Beyond the last crossing the filter is zero.
inline float FilterTap(const float* tab, size_t n, double x)
{
    const double p = x * kFilterPhases;
    const size_t i = (size_t)p;
    if (i + 1 >= n)
        return 0.0f;

    const float f = (float)(p - (double)i);
    return tab[i] + f * (tab[i + 1] - tab[i]);
}

} // namespace

WavReader::WavReader()
    : m_file(INVALID_HANDLE_VALUE), m_srcRate(0), m_srcChannels(0), m_srcBits(0),
      m_srcFrameBytes(0), m_srcIsFloat(false), m_dataOffset(0), m_dataBytes(0),
      m_dataRead(0), m_dataEof(false), m_srcTotal(0), m_durationSeconds(0.0),
      m_stepInt(1), m_stepNum(0), m_stepDen(1), m_posInt(0), m_posRem(0),
      m_outIndex(0), m_outTotal(0), m_rawUsed(0), m_rawFilled(0),
      m_srcFirst(0), m_srcCount(0), m_srcCap(0), m_cutoff(1.0), m_half(0),
      m_passthrough(true)
{
}

WavReader::~WavReader()
{
    Close();
}

void WavReader::Close()
{
    if (m_file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_file);
        m_file = INVALID_HANDLE_VALUE;
    }

    m_srcRate = m_srcChannels = m_srcBits = m_srcFrameBytes = 0;
    m_srcIsFloat = false;
    m_dataOffset = 0;
    m_dataBytes = m_dataRead = 0;
    m_dataEof = false;
    m_srcTotal = 0;
    m_durationSeconds = 0.0;
    m_stepInt = 1;
    m_stepNum = 0;
    m_stepDen = 1;
    m_posInt = m_posRem = 0;
    m_outIndex = m_outTotal = 0;
    m_raw.clear();
    m_rawUsed = m_rawFilled = 0;
    m_src.clear();
    m_srcFirst = m_srcCount = 0;
    m_srcCap = 0;
    m_filter.clear();
    m_cutoff = 1.0;
    m_half = 0;
    m_passthrough = true;
}

bool WavReader::IsRiffWave(const std::wstring& path)
{
    if (path.empty())
        return false;

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    unsigned char hdr[12] = { 0 };
    DWORD got = 0;
    const BOOL ok = ReadFile(h, hdr, sizeof(hdr), &got, NULL);
    CloseHandle(h);

    if (!ok || got != sizeof(hdr))
        return false;

    // RF64 and BW64 are the same layout with 64-bit sizes in a ds64 chunk.
    const bool riff = memcmp(hdr, "RIFF", 4) == 0 ||
                      memcmp(hdr, "RF64", 4) == 0 ||
                      memcmp(hdr, "BW64", 4) == 0;
    return riff && memcmp(hdr + 8, "WAVE", 4) == 0;
}

bool WavReader::ReadExact(void* dst, DWORD bytes)
{
    DWORD got = 0;
    if (!ReadFile(m_file, dst, bytes, &got, NULL))
        return false;
    return got == bytes;
}

bool WavReader::ParseFormat(const unsigned char* fmt, size_t bytes, std::wstring& error)
{
    wchar_t buf[320];

    if (bytes < 16)
    {
        error = L"The WAV file has a truncated fmt chunk, so its format is unknown.";
        return false;
    }

    unsigned int tag  = Le16(fmt);
    m_srcChannels     = (int)Le16(fmt + 2);
    m_srcRate         = (int)Le32(fmt + 4);
    m_srcFrameBytes   = (int)Le16(fmt + 12);
    m_srcBits         = (int)Le16(fmt + 14);

    if (tag == kTagExtensible)
    {
        if (bytes < 40)
        {
            error = L"The WAV file says it is WAVE_FORMAT_EXTENSIBLE but its fmt "
                    L"chunk is too short to hold the format GUID.";
            return false;
        }

        const unsigned char* guid = fmt + 24;
        if (memcmp(guid + 4, kSubFormatTail, sizeof(kSubFormatTail)) != 0)
        {
            error = L"The WAV file uses a private format GUID that this reader does "
                    L"not recognize.";
            return false;
        }
        tag = Le32(guid);
    }

    if (tag == kTagIeeeFloat)
        m_srcIsFloat = true;
    else if (tag == kTagPcm)
        m_srcIsFloat = false;
    else
    {
        // Compressed WAVs land here. Media Foundation has decoders for them, so
        // the caller falling back is the right answer, not a hard stop.
        swprintf_s(buf, L"The WAV file uses compressed format 0x%04X, which needs a "
                        L"decoder rather than a plain PCM read.", tag);
        error = buf;
        return false;
    }

    if (m_srcChannels < 1)
    {
        error = L"The WAV file declares no audio channels.";
        return false;
    }
    if (m_srcRate < 1)
    {
        error = L"The WAV file declares a sample rate of zero.";
        return false;
    }

    const bool bitsOk = m_srcIsFloat ? (m_srcBits == 32 || m_srcBits == 64)
                                     : (m_srcBits == 8 || m_srcBits == 16 ||
                                        m_srcBits == 24 || m_srcBits == 32);
    if (!bitsOk)
    {
        swprintf_s(buf, L"The WAV file is %d-bit %s, which is not a PCM width this "
                        L"reader handles.", m_srcBits,
                   m_srcIsFloat ? L"float" : L"integer");
        error = buf;
        return false;
    }

    // A few writers leave block align at zero or set it short. The frame stride has
    // to cover every channel or the file cannot be walked at all.
    const int minStride = m_srcChannels * (m_srcBits / 8);
    if (m_srcFrameBytes < minStride)
        m_srcFrameBytes = minStride;

    return true;
}

bool WavReader::ParseHeader(std::wstring& error)
{
    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(m_file, &fileSize))
    {
        error = L"Could not read the size of the WAV file.";
        return false;
    }

    unsigned char riff[12];
    if (!ReadExact(riff, sizeof(riff)))
    {
        error = L"The file is too short to be a WAV file.";
        return false;
    }

    const bool rf64 = memcmp(riff, "RF64", 4) == 0 || memcmp(riff, "BW64", 4) == 0;
    if ((memcmp(riff, "RIFF", 4) != 0 && !rf64) || memcmp(riff + 8, "WAVE", 4) != 0)
    {
        error = L"This is not a RIFF/WAVE file.";
        return false;
    }

    unsigned long long ds64DataBytes = 0;
    bool haveDs64 = false;
    bool haveFmt  = false;
    bool haveData = false;
    unsigned long long dataBytes = 0;

    // Chunks come in any order and DAWs write plenty this does not care about
    // (LIST, bext, fact, cue, iXML), so walk the whole file by declared size
    // rather than assuming fmt is first or data is last.
    long long pos = 12;
    while (pos + 8 <= fileSize.QuadPart)
    {
        LARGE_INTEGER seek;
        seek.QuadPart = pos;
        if (!SetFilePointerEx(m_file, seek, NULL, FILE_BEGIN))
            break;

        unsigned char hdr[8];
        if (!ReadExact(hdr, sizeof(hdr)))
            break;

        unsigned long long size = Le32(hdr + 4);
        const long long body = pos + 8;

        if (memcmp(hdr, "ds64", 4) == 0 && rf64 && size >= 28)
        {
            unsigned char ds[28];
            if (!ReadExact(ds, sizeof(ds)))
                break;
            ds64DataBytes = Le64(ds + 8);
            haveDs64 = true;
        }
        else if (memcmp(hdr, "fmt ", 4) == 0 && !haveFmt)
        {
            unsigned char fmt[40] = { 0 };
            const DWORD want = (DWORD)(size < sizeof(fmt) ? size : sizeof(fmt));
            if (!ReadExact(fmt, want))
            {
                error = L"The WAV file ends inside its fmt chunk.";
                return false;
            }
            if (!ParseFormat(fmt, want, error))
                return false;
            haveFmt = true;
        }
        else if (memcmp(hdr, "data", 4) == 0 && !haveData)
        {
            m_dataOffset = body;
            dataBytes = size;

            // RF64 parks 0xFFFFFFFF here and keeps the real size in ds64. A plain
            // RIFF written by a streaming encoder sometimes leaves it at zero or
            // at 0xFFFFFFFF too, in which case the rest of the file is the audio.
            if (size == 0xFFFFFFFFull || size == 0)
                dataBytes = haveDs64 ? ds64DataBytes
                                     : (unsigned long long)(fileSize.QuadPart - body);

            const unsigned long long avail = (unsigned long long)(fileSize.QuadPart - body);
            if (dataBytes > avail)
                dataBytes = avail;      // truncated file: play what is there

            haveData = true;
        }

        if (haveFmt && haveData)
            break;

        if (size == 0xFFFFFFFFull && haveDs64)
            size = ds64DataBytes;       // so a fmt chunk after a huge data chunk is still reachable

        const long long next = body + (long long)size + (long long)(size & 1);
        if (next <= pos)
            break;                      // a malformed size that would loop forever
        pos = next;
    }

    if (!haveFmt)
    {
        error = L"The WAV file has no fmt chunk, so there is nothing saying what "
                L"format its audio is in.";
        return false;
    }
    if (!haveData)
    {
        error = L"The WAV file has no data chunk, so it holds no audio.";
        return false;
    }

    m_dataBytes = dataBytes;
    m_srcTotal  = (long long)(m_dataBytes / (unsigned long long)m_srcFrameBytes);
    if (m_srcTotal <= 0)
    {
        error = L"The WAV file's data chunk is empty.";
        return false;
    }

    m_durationSeconds = (double)m_srcTotal / (double)m_srcRate;
    return true;
}

void WavReader::BuildFilter()
{
    const int n = kZeroCrossings * kFilterPhases;
    m_filter.resize((size_t)n + 2);

    const double denom = BesselI0(kKaiserBeta);
    for (int i = 0; i <= n; i++)
    {
        const double x = (double)i / (double)kFilterPhases;   // zero crossings from the centre
        const double t = x / (double)kZeroCrossings;          // 0 at the centre, 1 at the edge
        const double w = BesselI0(kKaiserBeta * sqrt(1.0 - t * t)) / denom;
        const double s = (i == 0) ? 1.0 : sin(kPi * x) / (kPi * x);
        m_filter[(size_t)i] = (float)(s * w);
    }

    // Lets the interpolation read one entry past the last crossing without a branch.
    m_filter[(size_t)n + 1] = 0.0f;
}

bool WavReader::Open(const std::wstring& path, std::wstring& error)
{
    Close();

    if (path.empty())
    {
        error = L"No audio file given.";
        return false;
    }

    m_file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (m_file == INVALID_HANDLE_VALUE)
    {
        error = L"Could not open the audio file for reading.";
        return false;
    }

    if (!ParseHeader(error))
    {
        Close();
        return false;
    }

    // The step is exact: with g the common factor, every output sample sits at
    // n * M / L source frames, so a five hour file lands where it should.
    const long long g = Gcd((long long)m_srcRate, (long long)kSampleRate);
    const long long M = (long long)m_srcRate / g;
    const long long L = (long long)kSampleRate / g;

    m_stepInt = M / L;
    m_stepNum = M % L;
    m_stepDen = L;
    m_outTotal = (m_srcTotal * L + M / 2) / M;

    m_passthrough = (m_srcRate == kSampleRate);
    if (m_passthrough)
    {
        m_cutoff = 1.0;
        m_half = 0;
    }
    else
    {
        // Downsampling has to band limit to the output Nyquist; upsampling only
        // has to interpolate, so the cutoff stays at the input Nyquist.
        m_cutoff = (m_srcRate > kSampleRate)
                 ? kRolloff * (double)kSampleRate / (double)m_srcRate
                 : 1.0;
        m_half = (long long)ceil((double)kZeroCrossings / m_cutoff) + 1;
        BuildFilter();
    }

    m_srcCap = (size_t)(2 * m_half + 2) + 2 * kTrimSlack;
    m_src.assign(m_srcCap * 2, 0.0f);
    m_srcFirst = 0;
    m_srcCount = 0;

    size_t rawBytes = kRawBytes;
    if ((size_t)m_srcFrameBytes * 64 > rawBytes)
        rawBytes = (size_t)m_srcFrameBytes * 64;    // a wide multichannel frame still fits
    m_raw.assign(rawBytes, 0);
    m_rawUsed = m_rawFilled = 0;
    m_dataRead = 0;
    m_dataEof = false;

    LARGE_INTEGER seek;
    seek.QuadPart = m_dataOffset;
    if (!SetFilePointerEx(m_file, seek, NULL, FILE_BEGIN))
    {
        error = L"Could not seek to the audio data in the WAV file.";
        Close();
        return false;
    }
    return true;
}

bool WavReader::FillRaw(std::wstring& error)
{
    // Carry the partial frame at the tail back to the front.
    if (m_rawUsed > 0)
    {
        if (m_rawUsed < m_rawFilled)
            memmove(&m_raw[0], &m_raw[m_rawUsed], m_rawFilled - m_rawUsed);
        m_rawFilled -= m_rawUsed;
        m_rawUsed = 0;
    }

    const unsigned long long remain = (m_dataRead < m_dataBytes)
                                    ? (m_dataBytes - m_dataRead) : 0;
    const size_t room = m_raw.size() - m_rawFilled;
    const size_t want = (remain < (unsigned long long)room) ? (size_t)remain : room;
    if (want == 0)
        return true;

    DWORD got = 0;
    if (!ReadFile(m_file, &m_raw[m_rawFilled], (DWORD)want, &got, NULL))
    {
        error = L"Could not read the audio data from the WAV file.";
        return false;
    }

    m_rawFilled += got;
    m_dataRead  += got;
    if (got == 0)
        m_dataEof = true;   // shorter on disk than the header claimed

    return true;
}

bool WavReader::FillSource(long long endFrame, std::wstring& error)
{
    const int stride = m_srcFrameBytes;
    const int width  = m_srcBits / 8;

    while (m_srcFirst + m_srcCount < endFrame)
    {
        if (m_srcFirst + m_srcCount >= m_srcTotal || m_dataEof)
            break;                                  // the song ended; the rest reads as silence
        if (m_srcCount >= (long long)m_srcCap)
            break;                                  // window full, which only happens if trimming stopped

        if (m_rawFilled - m_rawUsed < (size_t)stride)
        {
            if (!FillRaw(error))
                return false;
            if (m_rawFilled - m_rawUsed < (size_t)stride)
            {
                m_dataEof = true;                   // the data chunk ended inside a frame
                break;
            }
        }

        const unsigned char* p = &m_raw[m_rawUsed];
        const float l = DecodeSample(p, m_srcBits, m_srcIsFloat);

        // Channels past the second are dropped and mono goes to both sides, which
        // is what the live loopback path does in audio/audiobuf.cpp.
        const float r = (m_srcChannels >= 2)
                      ? DecodeSample(p + width, m_srcBits, m_srcIsFloat) : l;
        m_rawUsed += (size_t)stride;

        const size_t slot = (size_t)m_srcCount * 2;
        m_src[slot + 0] = l;
        m_src[slot + 1] = r;
        m_srcCount++;
    }
    return true;
}

void WavReader::TrimSource(long long firstNeeded)
{
    long long dead = firstNeeded - m_srcFirst;
    if (dead < (long long)kTrimSlack)
        return;
    if (dead > m_srcCount)
        dead = m_srcCount;      // decoding is sequential, so the window never jumps ahead

    const size_t keep = (size_t)(m_srcCount - dead) * 2;
    if (keep > 0)
        memmove(&m_src[0], &m_src[(size_t)dead * 2], keep * sizeof(float));

    m_srcFirst += dead;
    m_srcCount -= dead;
}

bool WavReader::Skip(long long count, std::wstring& error)
{
    float scratch[1024 * 2];
    while (count > 0)
    {
        const size_t want = (size_t)((count > 1024) ? 1024 : count);
        size_t got = 0;
        if (!ReadSamples(scratch, want, got, error))
            return false;
        if (got == 0)
            break;   // ran off the end of the song
        count -= (long long)got;
    }
    return true;
}
bool WavReader::ReadSamples(float* out, size_t count, size_t& written, std::wstring& error)
{
    written = 0;

    if (m_file == INVALID_HANDLE_VALUE)
    {
        error = L"The WAV file is not open.";
        return false;
    }
    if (!out)
    {
        error = L"No buffer given to read the audio into.";
        return false;
    }

    const float* tab = m_filter.empty() ? NULL : &m_filter[0];
    const size_t tabN = m_filter.size();
    const double c = m_cutoff;

    while (written < count && m_outIndex < m_outTotal)
    {
        const long long i0 = m_posInt;

        TrimSource(i0 - m_half);
        if (!FillSource(i0 + m_half + 2, error))
            return false;

        float l = 0.0f;
        float r = 0.0f;

        if (m_passthrough)
        {
            if (i0 >= m_srcFirst && i0 < m_srcFirst + m_srcCount)
            {
                const size_t slot = (size_t)(i0 - m_srcFirst) * 2;
                l = m_src[slot + 0];
                r = m_src[slot + 1];
            }
        }
        else
        {
            const double frac = (double)m_posRem / (double)m_stepDen;

            // Taps at and before the centre frame. Running off the front of the
            // file is silence, which is the correct edge for a symmetric filter.
            double xf = frac * c;
            for (long long j = 0; j <= m_half; j++, xf += c)
            {
                if (xf >= (double)kZeroCrossings)
                    break;
                const long long idx = i0 - j;
                if (idx < m_srcFirst)
                    break;

                const float h = FilterTap(tab, tabN, xf);
                const size_t slot = (size_t)(idx - m_srcFirst) * 2;
                l += h * m_src[slot + 0];
                r += h * m_src[slot + 1];
            }

            // Taps after the centre frame, stopping at the end of the song.
            xf = (1.0 - frac) * c;
            for (long long j = 0; j <= m_half; j++, xf += c)
            {
                if (xf >= (double)kZeroCrossings)
                    break;
                const long long idx = i0 + 1 + j;
                if (idx >= m_srcFirst + m_srcCount)
                    break;

                const float h = FilterTap(tab, tabN, xf);
                const size_t slot = (size_t)(idx - m_srcFirst) * 2;
                l += h * m_src[slot + 0];
                r += h * m_src[slot + 1];
            }

            // Stretching the filter by 1/c scaled its gain by the same amount.
            l = (float)(l * c);
            r = (float)(r * c);
        }

        out[written * 2 + 0] = l;
        out[written * 2 + 1] = r;
        written++;

        m_posInt += m_stepInt;
        m_posRem += m_stepNum;
        if (m_posRem >= m_stepDen)
        {
            m_posRem -= m_stepDen;
            m_posInt++;
        }
        m_outIndex++;
    }
    return true;
}

} // namespace offline
