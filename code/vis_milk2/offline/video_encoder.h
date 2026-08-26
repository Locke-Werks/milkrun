#ifndef __MILKRUN_VIDEO_ENCODER_H__
#define __MILKRUN_VIDEO_ENCODER_H__ 1

#include <windows.h>
#include <d3d9.h>
#include <string>
#include <vector>

namespace offline {

// One coded access unit, in Annex-B form (start codes, parameter sets inline).
// That is what NVENC emits and what Media Foundation's MP4 sink accepts as
// MFVideoFormat_HEVC, so nothing has to repackage NAL units in between.
struct EncodedPacket
{
    const unsigned char* data;
    size_t               size;
    long long            frameIndex;   // presentation order
    bool                 keyframe;
};

// Returns false to abort the render; fills 'error' when it does.
typedef bool (*PacketSink)(void* ctx, const EncodedPacket& pkt, std::wstring& error);

struct EncoderInitParams
{
    // NVENC opens a session directly on the D3D9 device the visualizer renders
    // with, so no interop layer is needed. Backends that do not want it ignore it.
    IDirect3DDevice9* device = NULL;

    int width  = 0;
    int height = 0;
    int fpsNum = 60;
    int fpsDen = 1;

    // True when frames arrive as A2R10G10B10 rather than X8R8G8B8. Independent of
    // the output, which is always Main10: an 8-bit input is widened by the encoder.
    bool tenBitInput = false;

    int bitrateKbps = 0;   // 0 selects constant quality instead
    int quality     = 24;  // constant-quality level, lower is better

    // HDR10 signalling. The pixels are already converted by the time they arrive;
    // this only controls what gets written into the VUI and the SEI payloads.
    bool  pq2020           = false;
    float diffuseWhiteNits = 203.0f;
    float peakNits         = 1000.0f;

    PacketSink sink    = NULL;
    void*      sinkCtx = NULL;
};

class IVideoEncoder
{
public:
    virtual ~IVideoEncoder() {}

    virtual bool Init(const EncoderInitParams& params, std::wstring& error) = 0;
    virtual void Shutdown() = 0;

    // Hands one frame to the encoder. Packets are delivered through the sink and
    // may lag the submission, since the encoder reorders for B-frames.
    virtual bool EncodeFrame(const void* bits, int pitchBytes,
                             long long frameIndex, std::wstring& error) = 0;

    // Drains everything still in flight. Must be called before Shutdown.
    virtual bool Flush(std::wstring& error) = 0;

    // VPS/SPS/PPS, for the muxer's MF_MT_MPEG_SEQUENCE_HEADER. Without this the
    // MP4 sink fails at Finalize with 0xc00d4a45.
    virtual bool GetSequenceParams(std::vector<unsigned char>& out) = 0;

    virtual const wchar_t* Name() const = 0;
};

} // namespace offline

#endif
