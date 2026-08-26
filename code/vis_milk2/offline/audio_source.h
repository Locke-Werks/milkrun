#ifndef __MILKRUN_AUDIO_SOURCE_H__
#define __MILKRUN_AUDIO_SOURCE_H__ 1

#include <windows.h>
#include <string>
#include <vector>

struct IMFSourceReader;

namespace offline {

class WavReader;

// Decodes the song and hands the visualizer one 576-sample window per frame.
//
// Two things matter here. First the rate: AnalyzeNewSound treats its 576 samples
// as spanning 0-22050 Hz, so the stream is resampled to 44100 Hz and the whole
// frequency scale would shift otherwise.
//
// Second the alignment: frame N is displayed across [N/fps, (N+1)/fps), so its
// window starts at N/fps rather than ending there. The audio a frame reacts to
// then falls inside the interval that frame is on screen, instead of trailing it
// by the length of the window.
//
// Reads are strictly forward, so this streams through a small ring rather than
// holding the decoded song in memory: an hour-long set costs the same as a single.
class AudioSource
{
public:
    AudioSource();
    ~AudioSource();

    // Returns false and fills 'error' if the file cannot be opened or decoded.
    bool Open(const std::wstring& path, std::wstring& error);
    void Close();

    double DurationSeconds() const { return m_durationSeconds; }

    // Total frames needed to cover the song at this rate, so the video ends when
    // the song does, with nothing padded on either end.
    long long FrameCount(int fpsNum, int fpsDen) const;

    // Fills 576 bytes per channel in the 8-bit signed-in-unsigned form the
    // visualizer expects. frameIndex must not go backwards.
    bool FillFrameWindow(long long frameIndex, int fpsNum, int fpsDen,
                         unsigned char* outL, unsigned char* outR);

    static const int kSampleRate = 44100;
    static const int kWindowSamples = 576;

private:
    // Pulls decoded frames forward until the ring covers through endSample.
    bool EnsureBuffered(long long endSample);

    IMFSourceReader*   m_reader;
    bool               m_mfStarted;
    double             m_durationSeconds;
    long long          m_totalSamples;

    // Ring of interleaved stereo float, addressed in absolute sample positions.
    std::vector<float> m_ring;          // 2 floats per sample
    long long          m_ringFirst;     // absolute index of the oldest sample held
    long long          m_ringCount;     // samples currently held
    bool               m_eof;
    bool               m_sourceIsFloat;   // false when we fell back to 16-bit PCM

    // Non-null when the file is a WAVE we read ourselves, which is the only way to
    // reach bit depths Media Foundation refuses.
    WavReader*         m_wav;
};

} // namespace offline

#endif
