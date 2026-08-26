#include "nvenc_encoder.h"
#include "../../third_party/nvenc/nvEncodeAPI.h"

#include <vector>
#include <deque>
#include <string>

namespace offline {

namespace {

// This project builds x86, because ns-eel2's expression JIT is x86-only. Keep both
// names anyway so an x64 build would still find the runtime.
#ifdef _WIN64
const wchar_t kNvEncDll[] = L"nvEncodeAPI64.dll";
#else
const wchar_t kNvEncDll[] = L"nvEncodeAPI.dll";
#endif

typedef NVENCSTATUS (NVENCAPI *PFN_CreateInstance)(NV_ENCODE_API_FUNCTION_LIST*);
typedef NVENCSTATUS (NVENCAPI *PFN_GetMaxVersion)(uint32_t*);

// NvEncodeAPIGetMaxSupportedVersion packs the driver's API level as
// (major << 4) | minor, which is a different layout from NVENCAPI_VERSION.
// Comparing the two directly makes every driver look too old.
const uint32_t kRequiredApiVersion =
    (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;

std::wstring Describe(NVENCSTATUS s, const wchar_t* what)
{
    wchar_t buf[256];
    swprintf_s(buf, L"NVENC %s failed (status %d).", what, (int)s);
    return buf;
}

} // namespace

struct NvencEncoder::Impl
{
    HMODULE                     dll;
    NV_ENCODE_API_FUNCTION_LIST fn;
    void*                       encoder;

    EncoderInitParams           params;
    NV_ENC_BUFFER_FORMAT        bufferFormat;
    int                         bytesPerPixel;

    // Rate control lookahead makes the encoder hold frames back, so submissions
    // and outputs do not pair up one for one. Every in-flight frame owns a slot,
    // and slots drain in submission order once the encoder says output is ready.
    struct Slot
    {
        NV_ENC_INPUT_PTR  input;
        NV_ENC_OUTPUT_PTR output;
        Slot() : input(NULL), output(NULL) {}
    };
    std::vector<Slot>  slots;
    std::deque<size_t> inFlight;
    size_t             nextSlot;

    std::vector<unsigned char> sequenceParams;

    Impl()
        : dll(NULL), encoder(NULL), bufferFormat(NV_ENC_BUFFER_FORMAT_ARGB),
          bytesPerPixel(4), nextSlot(0)
    {
        memset(&fn, 0, sizeof(fn));
    }
};

NvencEncoder::NvencEncoder() : m_impl(new Impl()) {}

NvencEncoder::~NvencEncoder()
{
    Shutdown();
    delete m_impl;
    m_impl = NULL;
}

bool NvencEncoder::IsAvailable(IDirect3DDevice9* device, std::wstring* whyNot)
{
    (void)device;

    HMODULE dll = LoadLibraryW(kNvEncDll);
    if (!dll)
    {
        if (whyNot) *whyNot = L"No NVIDIA encoder runtime on this machine.";
        return false;
    }

    bool ok = false;
    PFN_GetMaxVersion getMax =
        (PFN_GetMaxVersion)GetProcAddress(dll, "NvEncodeAPIGetMaxSupportedVersion");

    uint32_t maxVersion = 0;
    if (getMax && getMax(&maxVersion) == NV_ENC_SUCCESS)
    {
        if (maxVersion >= kRequiredApiVersion)
            ok = true;
        else if (whyNot)
            *whyNot = L"The NVIDIA driver predates this build's encoder API. Update the driver.";
    }
    else if (whyNot)
    {
        *whyNot = L"The NVIDIA encoder runtime would not report its version.";
    }

    FreeLibrary(dll);
    return ok;
}

bool NvencEncoder::Init(const EncoderInitParams& params, std::wstring& error)
{
    Shutdown();
    Impl& d = *m_impl;
    d.params = params;

    if (!params.device)
    {
        error = L"NVENC needs the Direct3D device the frames were rendered on.";
        return false;
    }

    d.dll = LoadLibraryW(kNvEncDll);
    if (!d.dll)
    {
        error = L"No NVIDIA encoder runtime on this machine.";
        return false;
    }

    PFN_GetMaxVersion getMax =
        (PFN_GetMaxVersion)GetProcAddress(d.dll, "NvEncodeAPIGetMaxSupportedVersion");
    uint32_t maxVersion = 0;
    if (!getMax || getMax(&maxVersion) != NV_ENC_SUCCESS)
    {
        error = L"The NVIDIA encoder runtime would not report its version.";
        Shutdown();
        return false;
    }
    if (kRequiredApiVersion > maxVersion)
    {
        wchar_t buf[256];
        swprintf_s(buf,
            L"The NVIDIA driver supports encoder API %u.%u but this build needs %u.%u. "
            L"Update the driver.",
            maxVersion >> 4, maxVersion & 0xF,
            (unsigned)NVENCAPI_MAJOR_VERSION, (unsigned)NVENCAPI_MINOR_VERSION);
        error = buf;
        Shutdown();
        return false;
    }

    PFN_CreateInstance createInstance =
        (PFN_CreateInstance)GetProcAddress(d.dll, "NvEncodeAPICreateInstance");
    if (!createInstance)
    {
        error = L"The NVIDIA encoder runtime is missing its entry point.";
        Shutdown();
        return false;
    }

    d.fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS s = createInstance(&d.fn);
    if (s != NV_ENC_SUCCESS)
    {
        error = Describe(s, L"instance creation");
        Shutdown();
        return false;
    }

    // NVENC takes a Direct3D 9 device directly, which is why the visualizer's own
    // device can be handed straight over with no interop layer in between.
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open;
    memset(&open, 0, sizeof(open));
    open.version    = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    open.device     = params.device;
    open.apiVersion = NVENCAPI_VERSION;

    s = d.fn.nvEncOpenEncodeSessionEx(&open, &d.encoder);
    if (s != NV_ENC_SUCCESS)
    {
        d.encoder = NULL;
        error = Describe(s, L"session open");
        Shutdown();
        return false;
    }

    // Refuse outright rather than quietly shipping 8-bit when Main10 was asked for.
    NV_ENC_CAPS_PARAM cap;
    memset(&cap, 0, sizeof(cap));
    cap.version     = NV_ENC_CAPS_PARAM_VER;
    cap.capsToQuery = NV_ENC_CAPS_SUPPORT_10BIT_ENCODE;
    int capValue = 0;
    if (d.fn.nvEncGetEncodeCaps(d.encoder, NV_ENC_CODEC_HEVC_GUID, &cap, &capValue) != NV_ENC_SUCCESS
        || capValue == 0)
    {
        error = L"This GPU's encoder cannot produce 10-bit HEVC.";
        Shutdown();
        return false;
    }

    // Both formats are 32 bits per pixel and match the D3D9 back buffer layouts
    // bit for bit, so the readback can be copied across without conversion.
    d.bufferFormat  = params.tenBitInput ? NV_ENC_BUFFER_FORMAT_ARGB10
                                         : NV_ENC_BUFFER_FORMAT_ARGB;
    d.bytesPerPixel = 4;

    const GUID presetGuid = NV_ENC_PRESET_P6_GUID;

    NV_ENC_PRESET_CONFIG presetConfig;
    memset(&presetConfig, 0, sizeof(presetConfig));
    presetConfig.version           = NV_ENC_PRESET_CONFIG_VER;
    presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;
    s = d.fn.nvEncGetEncodePresetConfigEx(d.encoder, NV_ENC_CODEC_HEVC_GUID, presetGuid,
                                          NV_ENC_TUNING_INFO_HIGH_QUALITY, &presetConfig);
    if (s != NV_ENC_SUCCESS)
    {
        error = Describe(s, L"preset lookup");
        Shutdown();
        return false;
    }

    NV_ENC_CONFIG cfg = presetConfig.presetCfg;
    cfg.version     = NV_ENC_CONFIG_VER;
    cfg.profileGUID = NV_ENC_HEVC_PROFILE_MAIN10_GUID;

    NV_ENC_CONFIG_HEVC* hevc = &cfg.encodeCodecConfig.hevcConfig;
    hevc->outputBitDepth = NV_ENC_BIT_DEPTH_10;
    hevc->inputBitDepth  = params.tenBitInput ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;

    // Repeat the parameter sets on every IDR so the muxer can pick them up and the
    // file stays decodable if it is ever cut.
    hevc->repeatSPSPPS = 1;

    // An IDR every two seconds keeps seeking responsive for very little size.
    int gop = 120;
    if (params.fpsDen > 0 && params.fpsNum > 0)
        gop = params.fpsNum * 2 / params.fpsDen;
    if (gop <= 0) gop = 120;
    cfg.gopLength   = gop;
    hevc->idrPeriod = gop;

    // No B-frames yet. Output then arrives in display order with presentation and
    // decode timestamps identical, which keeps the muxer straightforward. Enabling
    // them is a size win but needs composition offsets handled downstream.
    cfg.frameIntervalP = 1;

    cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    if (params.bitrateKbps > 0)
    {
        cfg.rcParams.averageBitRate = (uint32_t)params.bitrateKbps * 1000;
        cfg.rcParams.maxBitRate     = (uint32_t)params.bitrateKbps * 1500;
    }
    else
    {
        cfg.rcParams.targetQuality  = (uint8_t)params.quality;
        cfg.rcParams.averageBitRate = 0;
        cfg.rcParams.maxBitRate     = 0;
    }

    if (params.pq2020)
    {
        // The pixels are already BT.2020 with the PQ curve by the time they get
        // here; this is only the signalling that tells a display to treat them so.
        NV_ENC_CONFIG_HEVC_VUI_PARAMETERS* vui = &hevc->hevcVUIParameters;
        vui->videoSignalTypePresentFlag   = 1;
        vui->videoFullRangeFlag           = 0;    // limited range, as HDR10 expects
        vui->colourDescriptionPresentFlag = 1;
        vui->colourPrimaries              = NV_ENC_VUI_COLOR_PRIMARIES_BT2020;
        vui->transferCharacteristics      = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SMPTE2084;
        vui->colourMatrix                 = NV_ENC_VUI_MATRIX_COEFFS_BT2020_NCL;
    }

    NV_ENC_INITIALIZE_PARAMS init;
    memset(&init, 0, sizeof(init));
    init.version      = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID   = NV_ENC_CODEC_HEVC_GUID;
    init.presetGUID   = presetGuid;
    init.tuningInfo   = NV_ENC_TUNING_INFO_HIGH_QUALITY;
    init.encodeWidth  = params.width;
    init.encodeHeight = params.height;
    init.darWidth     = params.width;
    init.darHeight    = params.height;
    init.frameRateNum = params.fpsNum;
    init.frameRateDen = params.fpsDen;
    init.enablePTD    = 1;             // let NVENC choose picture types
    init.encodeConfig = &cfg;

    s = d.fn.nvEncInitializeEncoder(d.encoder, &init);
    if (s != NV_ENC_SUCCESS)
    {
        error = Describe(s, L"encoder initialization");
        Shutdown();
        return false;
    }

    // Deep enough to cover whatever the rate controller wants to hold back.
    const size_t kSlots = 16;
    d.slots.resize(kSlots);
    for (size_t i = 0; i < kSlots; i++)
    {
        NV_ENC_CREATE_INPUT_BUFFER inBuf;
        memset(&inBuf, 0, sizeof(inBuf));
        inBuf.version   = NV_ENC_CREATE_INPUT_BUFFER_VER;
        inBuf.width     = params.width;
        inBuf.height    = params.height;
        inBuf.bufferFmt = d.bufferFormat;
        if (d.fn.nvEncCreateInputBuffer(d.encoder, &inBuf) != NV_ENC_SUCCESS)
        {
            error = L"NVENC would not allocate its input buffers.";
            Shutdown();
            return false;
        }
        d.slots[i].input = inBuf.inputBuffer;

        NV_ENC_CREATE_BITSTREAM_BUFFER outBuf;
        memset(&outBuf, 0, sizeof(outBuf));
        outBuf.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        if (d.fn.nvEncCreateBitstreamBuffer(d.encoder, &outBuf) != NV_ENC_SUCCESS)
        {
            error = L"NVENC would not allocate its output buffers.";
            Shutdown();
            return false;
        }
        d.slots[i].output = outBuf.bitstreamBuffer;
    }

    // Collect the parameter sets now; the muxer needs them before the first frame.
    unsigned char spsBuf[1024];
    NV_ENC_SEQUENCE_PARAM_PAYLOAD sp;
    memset(&sp, 0, sizeof(sp));
    uint32_t spsSize = 0;
    sp.version              = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    sp.inBufferSize         = sizeof(spsBuf);
    sp.spsppsBuffer         = spsBuf;
    sp.outSPSPPSPayloadSize = &spsSize;
    if (d.fn.nvEncGetSequenceParams(d.encoder, &sp) == NV_ENC_SUCCESS && spsSize > 0)
        d.sequenceParams.assign(spsBuf, spsBuf + spsSize);

    return true;
}

void NvencEncoder::Shutdown()
{
    if (!m_impl)
        return;
    Impl& d = *m_impl;

    if (d.encoder)
    {
        for (size_t i = 0; i < d.slots.size(); i++)
        {
            if (d.slots[i].input)  d.fn.nvEncDestroyInputBuffer(d.encoder, d.slots[i].input);
            if (d.slots[i].output) d.fn.nvEncDestroyBitstreamBuffer(d.encoder, d.slots[i].output);
        }
        d.fn.nvEncDestroyEncoder(d.encoder);
        d.encoder = NULL;
    }
    d.slots.clear();
    d.inFlight.clear();
    d.nextSlot = 0;
    d.sequenceParams.clear();

    if (d.dll) { FreeLibrary(d.dll); d.dll = NULL; }
}

bool NvencEncoder::DrainOutput(void* bitstreamBuffer, long long /*frameIndex*/, std::wstring& error)
{
    Impl& d = *m_impl;

    NV_ENC_LOCK_BITSTREAM lock;
    memset(&lock, 0, sizeof(lock));
    lock.version         = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = bitstreamBuffer;
    lock.doNotWait       = 0;

    NVENCSTATUS s = d.fn.nvEncLockBitstream(d.encoder, &lock);
    if (s != NV_ENC_SUCCESS)
    {
        error = Describe(s, L"bitstream lock");
        return false;
    }

    bool ok = true;
    if (d.params.sink)
    {
        EncodedPacket pkt;
        pkt.data       = (const unsigned char*)lock.bitstreamBufferPtr;
        pkt.size       = lock.bitstreamSizeInBytes;
        pkt.frameIndex = (long long)lock.outputTimeStamp;
        pkt.keyframe   = (lock.pictureType == NV_ENC_PIC_TYPE_IDR ||
                          lock.pictureType == NV_ENC_PIC_TYPE_I);
        ok = d.params.sink(d.params.sinkCtx, pkt, error);
    }

    d.fn.nvEncUnlockBitstream(d.encoder, bitstreamBuffer);
    return ok;
}

bool NvencEncoder::EncodeFrame(const void* bits, int pitchBytes,
                               long long frameIndex, std::wstring& error)
{
    Impl& d = *m_impl;
    if (!d.encoder)
    {
        error = L"The encoder is not running.";
        return false;
    }

    // Reuse the oldest slot once it has drained. With a 16-deep ring and no
    // B-frames the encoder never holds that many, but cope if it ever does.
    if (d.inFlight.size() >= d.slots.size())
    {
        const size_t oldest = d.inFlight.front();
        d.inFlight.pop_front();
        if (!DrainOutput(d.slots[oldest].output, frameIndex, error))
            return false;
    }

    const size_t slot = d.nextSlot;
    d.nextSlot = (d.nextSlot + 1) % d.slots.size();

    NV_ENC_LOCK_INPUT_BUFFER lockIn;
    memset(&lockIn, 0, sizeof(lockIn));
    lockIn.version     = NV_ENC_LOCK_INPUT_BUFFER_VER;
    lockIn.inputBuffer = d.slots[slot].input;
    NVENCSTATUS s = d.fn.nvEncLockInputBuffer(d.encoder, &lockIn);
    if (s != NV_ENC_SUCCESS)
    {
        error = Describe(s, L"input lock");
        return false;
    }

    // The staging surface and the encoder buffer rarely share a pitch, so the copy
    // goes row by row rather than in one block.
    const int rowBytes = d.params.width * d.bytesPerPixel;
    const unsigned char* src = (const unsigned char*)bits;
    unsigned char* dst = (unsigned char*)lockIn.bufferDataPtr;
    for (int y = 0; y < d.params.height; y++)
        memcpy(dst + (size_t)y * lockIn.pitch, src + (size_t)y * pitchBytes, rowBytes);

    d.fn.nvEncUnlockInputBuffer(d.encoder, d.slots[slot].input);

    NV_ENC_PIC_PARAMS pic;
    memset(&pic, 0, sizeof(pic));
    pic.version         = NV_ENC_PIC_PARAMS_VER;
    pic.inputBuffer     = d.slots[slot].input;
    pic.outputBitstream = d.slots[slot].output;
    pic.bufferFmt       = d.bufferFormat;
    pic.pictureStruct   = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputWidth      = d.params.width;
    pic.inputHeight     = d.params.height;
    pic.inputPitch      = d.params.width;
    pic.inputTimeStamp  = (uint64_t)frameIndex;

    s = d.fn.nvEncEncodePicture(d.encoder, &pic);

    if (s == NV_ENC_SUCCESS)
    {
        // Everything queued, this frame included, now has output waiting.
        d.inFlight.push_back(slot);
        while (!d.inFlight.empty())
        {
            const size_t ready = d.inFlight.front();
            d.inFlight.pop_front();
            if (!DrainOutput(d.slots[ready].output, frameIndex, error))
                return false;
        }
        return true;
    }

    if (s == NV_ENC_ERR_NEED_MORE_INPUT)
    {
        d.inFlight.push_back(slot);
        return true;
    }

    error = Describe(s, L"encode");
    return false;
}

bool NvencEncoder::Flush(std::wstring& error)
{
    Impl& d = *m_impl;
    if (!d.encoder)
        return true;

    NV_ENC_PIC_PARAMS eos;
    memset(&eos, 0, sizeof(eos));
    eos.version        = NV_ENC_PIC_PARAMS_VER;
    eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;

    const NVENCSTATUS s = d.fn.nvEncEncodePicture(d.encoder, &eos);
    if (s != NV_ENC_SUCCESS && s != NV_ENC_ERR_NEED_MORE_INPUT)
    {
        error = Describe(s, L"flush");
        return false;
    }

    while (!d.inFlight.empty())
    {
        const size_t slot = d.inFlight.front();
        d.inFlight.pop_front();
        if (!DrainOutput(d.slots[slot].output, -1, error))
            return false;
    }
    return true;
}

bool NvencEncoder::GetSequenceParams(std::vector<unsigned char>& out)
{
    if (!m_impl || m_impl->sequenceParams.empty())
        return false;
    out = m_impl->sequenceParams;
    return true;
}

} // namespace offline
