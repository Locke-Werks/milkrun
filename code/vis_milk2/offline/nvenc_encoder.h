#ifndef __MILKRUN_NVENC_ENCODER_H__
#define __MILKRUN_NVENC_ENCODER_H__ 1

#include "video_encoder.h"

namespace offline {

// HEVC Main10 through NVENC, opened directly on the visualizer's D3D9 device.
//
// The DLL is loaded at run time rather than linked, so the binary still starts on
// a machine with no NVIDIA GPU; Init just fails and the caller falls back.
class NvencEncoder : public IVideoEncoder
{
public:
    NvencEncoder();
    virtual ~NvencEncoder();

    // True if the runtime is present and can encode HEVC Main10 here. Cheap enough
    // to call before committing to this backend.
    static bool IsAvailable(IDirect3DDevice9* device, std::wstring* whyNot);

    virtual bool Init(const EncoderInitParams& params, std::wstring& error);
    virtual void Shutdown();
    virtual bool EncodeFrame(const void* bits, int pitchBytes,
                             long long frameIndex, std::wstring& error);
    virtual bool Flush(std::wstring& error);
    virtual bool GetSequenceParams(std::vector<unsigned char>& out);
    virtual const wchar_t* Name() const { return L"NVENC"; }

private:
    // Drains one coded packet from the given bitstream buffer to the sink.
    bool DrainOutput(void* bitstreamBuffer, long long frameIndex, std::wstring& error);

    struct Impl;
    Impl* m_impl;
};

} // namespace offline

#endif
