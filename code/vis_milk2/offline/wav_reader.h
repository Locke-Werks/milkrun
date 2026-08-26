#ifndef __MILKRUN_WAV_READER_H__
#define __MILKRUN_WAV_READER_H__ 1

#include <windows.h>
#include <string>
#include <vector>

namespace offline {

// Reads RIFF/WAVE off the disk directly, because Media Foundation's PCM support
// stops at 32-bit float: a 192000 Hz 64-bit float stereo WAV, which is an ordinary
// Nuendo or Pro Tools export, comes back as "Windows cannot decode this file".
//
// Output is always 44100 Hz interleaved stereo float, the same as AudioSource
// asks Media Foundation for, because CPluginShell::AnalyzeNewSound treats its 576
// samples as spanning 0-22050 Hz and every band would shift at another rate.
// Channels past the second are dropped and mono is duplicated to both sides, which
// is what the live loopback path does in audio/audiobuf.cpp.
//
// Rate conversion is a Kaiser-windowed sinc rather than picking nearest samples:
// 192000 to 44100 with no lowpass folds everything above 22 kHz back down into the
// band, and this feeds an FFT that would show every bit of it.
//
// Reads are forward only and stream through a window a few hundred frames wide, so
// a 300 MB file costs no more memory than a 3 MB one.
class WavReader
{
public:
    WavReader();
    ~WavReader();

    WavReader(const WavReader&) = delete;
    WavReader& operator=(const WavReader&) = delete;

    // Cheap header sniff, so a caller can choose this over Media Foundation without
    // paying for a full open. True for RIFF/WAVE and for the RF64 and BW64 forms
    // that carry files past 4 GB.
    static bool IsRiffWave(const std::wstring& path);

    // Returns false and fills 'error' if this is not a WAVE it can decode. A
    // compressed WAV (ADPCM, mu-law) fails here on purpose so the caller can fall
    // back to Media Foundation, which has decoders for those.
    bool Open(const std::wstring& path, std::wstring& error);
    void Close();

    bool IsOpen() const { return m_file != INVALID_HANDLE_VALUE; }

    double DurationSeconds() const { return m_durationSeconds; }

    // Length of the converted stream, in 44100 Hz stereo samples.
    long long TotalSamples() const { return m_outTotal; }

    // What the file actually holds. Only for error and log text.
    int  SourceSampleRate()    const { return m_srcRate; }
    int  SourceChannels()      const { return m_srcChannels; }
    int  SourceBitsPerSample() const { return m_srcBits; }
    bool SourceIsFloat()       const { return m_srcIsFloat; }

    // Writes up to 'count' interleaved stereo samples (2 floats each) into 'out'
    // and reports how many it wrote. A short count means the song ended; reading on
    // writes nothing. Returns false only on a disk read error, which is fatal.
    bool ReadSamples(float* out, size_t count, size_t& written, std::wstring& error);

    // Advances past 'count' output samples without returning them, for starting a
    // render partway into a song. Reads and discards rather than seeking, because
    // the resampler carries state that a raw file seek would invalidate.
    bool Skip(long long count, std::wstring& error);

    static const int kSampleRate = 44100;

private:
    bool ReadExact(void* dst, DWORD bytes);
    bool ParseHeader(std::wstring& error);
    bool ParseFormat(const unsigned char* fmt, size_t bytes, std::wstring& error);
    void BuildFilter();

    // Pulls the next block of the data chunk off the disk.
    bool FillRaw(std::wstring& error);

    // Decodes source frames forward until 'endFrame' is in the window or the data
    // chunk runs out. Frames past the end are silence, not an error.
    bool FillSource(long long endFrame, std::wstring& error);

    // Drops frames the filter can no longer reach, so the window stays small.
    void TrimSource(long long firstNeeded);

    HANDLE m_file;

    // Straight out of the fmt chunk. m_srcBits is the container width, so 24-bit
    // samples padded into 32-bit containers read as 32.
    int  m_srcRate;
    int  m_srcChannels;
    int  m_srcBits;
    int  m_srcFrameBytes;    // block align: the stride from one frame to the next
    bool m_srcIsFloat;

    long long          m_dataOffset;   // byte offset of the first frame
    unsigned long long m_dataBytes;    // size of the data chunk
    unsigned long long m_dataRead;     // bytes of it consumed so far
    bool               m_dataEof;
    long long          m_srcTotal;     // frames the data chunk holds
    double             m_durationSeconds;

    // The output-to-input step, kept as an exact rational so a long file cannot
    // drift the way repeated addition of a double would.
    long long m_stepInt;
    long long m_stepNum;
    long long m_stepDen;
    long long m_posInt;      // source frame the next output sample sits on
    long long m_posRem;      // its fractional part, over m_stepDen
    long long m_outIndex;
    long long m_outTotal;

    // Raw bytes as they come off the disk.
    std::vector<unsigned char> m_raw;
    size_t m_rawUsed;
    size_t m_rawFilled;

    // Sliding window of decoded source frames, interleaved stereo float. Holds
    // frames [m_srcFirst, m_srcFirst + m_srcCount).
    std::vector<float> m_src;
    long long m_srcFirst;
    long long m_srcCount;
    size_t    m_srcCap;

    // One side of the windowed sinc; it is symmetric about the centre.
    std::vector<float> m_filter;
    double    m_cutoff;      // 1.0 unless downsampling, where it is the lowpass
    long long m_half;        // taps per side, in source frames
    bool      m_passthrough; // the file is already at 44100, so nothing to filter
};

} // namespace offline

#endif
