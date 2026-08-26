#ifndef __MILKRUN_MF_ENCODER_H__
#define __MILKRUN_MF_ENCODER_H__ 1

#include "video_encoder.h"

struct IMFSample;
struct IMFActivate;

namespace offline {

// HEVC through whichever encoder MFT Windows has registered, for machines with no
// NVIDIA GPU. AMD's AMF driver and Intel's Quick Sync driver each register one, so
// this single path covers both vendors; it works on NVIDIA too, just with less
// control over the encode than the NVENC backend has.
//
// No encoder MFT accepts RGB, so the packed 32bpp back buffer readback is converted
// to NV12 or P010 on the CPU here. That costs a few milliseconds a frame, and only
// on machines that have no better option.
class MfEncoder : public IVideoEncoder
{
public:
    MfEncoder();
    virtual ~MfEncoder();

    // True if an HEVC encoder MFT is registered at all. It says nothing about
    // 10-bit: whether Main10 is on offer is only known once a type is negotiated.
    static bool IsAvailable(std::wstring* whyNot);

    virtual bool Init(const EncoderInitParams& params, std::wstring& error);
    virtual void Shutdown();
    virtual bool EncodeFrame(const void* bits, int pitchBytes,
                             long long frameIndex, std::wstring& error);
    virtual bool Flush(std::wstring& error);
    virtual bool GetSequenceParams(std::vector<unsigned char>& out);
    virtual const wchar_t* Name() const;

    // Set when Main10 was asked for and the encoder would only take 8-bit Main. The
    // render still runs, but the file is not the one that was asked for, so the
    // caller is expected to say so rather than let it pass unmentioned.
    bool DowngradedToEightBit() const;

private:
    // Walks every registered encoder in preference order and keeps the first that
    // will actually negotiate. A machine with two GPUs registers an encoder for
    // each, and the one listed first is not necessarily the one that works.
    bool SelectTransform(std::wstring& error);
    bool TryCandidate(IMFActivate* candidate, bool tenBit, std::wstring& error);
    void ReleaseTransform();
    bool AttachDeviceManager();

    bool ConfigureTypes(bool tenBit, std::wstring& error);
    void ConfigureRateControl();
    bool BeginStreaming(std::wstring& error);
    bool RestartStreaming(std::wstring& error);

    // Reads VPS/SPS/PPS off the output type, and when the encoder will not describe
    // itself until it has coded something, runs a throwaway frame to make it.
    bool HarvestSequenceParams(std::wstring& error);

    bool BuildInputSample(const void* bits, int pitchBytes, long long frameIndex,
                          IMFSample** out, std::wstring& error);
    bool BuildBlackSample(IMFSample** out, std::wstring& error);
    bool SubmitSample(IMFSample* sample, std::wstring& error);

    // Pulls one coded frame if one is waiting; 'got' says whether one came out.
    bool PullOutput(bool* got, std::wstring& error);
    bool DrainPending(std::wstring& error);
    bool DrainToEndOfStream(std::wstring& error);
    bool DeliverSample(IMFSample* sample, std::wstring& error);

    struct Impl;
    Impl* m_impl;
};

} // namespace offline

#endif
