#ifndef __MILKRUN_MP4_MUXER_H__
#define __MILKRUN_MP4_MUXER_H__ 1

#include <windows.h>
#include <string>
#include <vector>

#include "video_encoder.h"

struct IMFSinkWriter;
struct IMFSourceReader;

namespace offline {

struct MuxerInitParams
{
    std::wstring outputPath;

    int width  = 0;
    int height = 0;
    int fpsNum = 60;
    int fpsDen = 1;

    // VPS/SPS/PPS from the encoder. Without these the MP4 sink fails at Finalize
    // with 0xc00d4a45, "required headers were not provided to the sink".
    std::vector<unsigned char> sequenceParams;

    // The song, muxed in as an AAC track so the file carries its own audio.
    std::wstring audioPath;

    // Where in the song the track should begin, matching the visuals when only a
    // section is being rendered.
    double startSeconds = 0.0;

    // Total frames the video will contain. The audio track is cut to the same
    // length, so a section render does not carry the rest of the song with it.
    long long totalFrames = 0;
    int audioBitrateKbps = 256;

    bool  pq2020   = false;
    float peakNits = 1000.0f;
};

class IMuxer
{
public:
    virtual ~IMuxer() {}

    virtual bool Init(const MuxerInitParams& params, std::wstring& error) = 0;

    // Video packets arrive in decode order. The muxer pulls audio forward to match
    // as it goes, so the two streams stay interleaved.
    virtual bool WriteVideoPacket(const EncodedPacket& pkt, std::wstring& error) = 0;

    // Writes any remaining audio and closes the file.
    virtual bool Finalize(std::wstring& error) = 0;

    // Abandons the file and deletes it, so a failed or cancelled render never
    // leaves a half-written mp4 behind.
    virtual void Abort() = 0;
};

// Media Foundation's MP4 sink.
//
// Video goes through untouched: the sink's input type is set equal to its output
// type, making it a pure muxer. MFVideoFormat_HEVC is defined as a bitstream with
// start codes and inline parameter sets, one frame per sample, which is exactly
// what NVENC emits with repeatSPSPPS on, so nothing has to repackage NAL units.
//
// Audio is decoded from the source file and re-encoded to AAC by the built-in
// encoder, which the sink inserts itself.
class Mp4Muxer : public IMuxer
{
public:
    Mp4Muxer();
    virtual ~Mp4Muxer();

    virtual bool Init(const MuxerInitParams& params, std::wstring& error);
    virtual bool WriteVideoPacket(const EncodedPacket& pkt, std::wstring& error);
    virtual bool Finalize(std::wstring& error);
    virtual void Abort();

private:
    // Feeds audio until its timeline reaches the given 100ns mark, or the song
    // runs out. Passing -1 drains the rest.
    bool PumpAudio(LONGLONG untilHns, std::wstring& error);

    struct Impl;
    Impl* m_impl;
};

} // namespace offline

#endif
