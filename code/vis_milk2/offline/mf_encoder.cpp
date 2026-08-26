#include "mf_encoder.h"

#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <icodecapi.h>
#include <d3d11.h>
#include <d3d10.h>

#include <vector>
#include <string>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "d3d11.lib")

namespace offline {

namespace {

const LONGLONG kHnsPerSecond = 10000000LL;

// A driver that has wedged should not take the whole render down with it.
const ULONGLONG kEventTimeoutMs = 30000;

std::wstring Hr(const wchar_t* what, HRESULT hr)
{
    wchar_t buf[256];
    swprintf_s(buf, L"%s (0x%08X).", what, (unsigned)hr);
    return buf;
}

// Naming the size matters here: an encoder that refuses one resolution will often
// take another, and the user is the only one who can pick a different one.
std::wstring RejectedFormat(const wchar_t* what, int width, int height, HRESULT hr)
{
    wchar_t buf[256];
    swprintf_s(buf, L"The video encoder would not %s at %dx%d (0x%08X).",
               what, width, height, (unsigned)hr);
    return buf;
}

// Non-constant-luminance RGB to YCbCr. BT.709 for SDR, and BT.2020 when the frame
// has already been converted to BT.2020 primaries with the PQ curve, so the pixels
// agree with what the stream is tagged with. Mixing the two tints every saturated
// colour and there is no way for a player to undo it.
struct YuvCoeffs
{
    float kr, kg, kb;
};

const YuvCoeffs kBt709  = { 0.2126f, 0.7152f, 0.0722f };
const YuvCoeffs kBt2020 = { 0.2627f, 0.6780f, 0.0593f };

struct Rgb
{
    float r, g, b;
};

inline int Clamp(int v, int lo, int hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

// Both readback formats are 32bpp with the channels packed into one little-endian
// DWORD: X8R8G8B8 as 0x00RRGGBB, A2R10G10B10 as (A<<30)|(R<<20)|(G<<10)|B.
inline Rgb Unpack(const unsigned char* row, int x, bool tenBit)
{
    const unsigned int v = ((const unsigned int*)row)[x];
    Rgb c;
    if (tenBit)
    {
        c.r = (float)((v >> 20) & 0x3FF) * (1.0f / 1023.0f);
        c.g = (float)((v >> 10) & 0x3FF) * (1.0f / 1023.0f);
        c.b = (float)( v        & 0x3FF) * (1.0f / 1023.0f);
    }
    else
    {
        c.r = (float)((v >> 16) & 0xFF) * (1.0f / 255.0f);
        c.g = (float)((v >>  8) & 0xFF) * (1.0f / 255.0f);
        c.b = (float)( v        & 0xFF) * (1.0f / 255.0f);
    }
    return c;
}

inline float Luma(const Rgb& c, const YuvCoeffs& k)
{
    return k.kr * c.r + k.kg * c.g + k.kb * c.b;
}

// Limited range throughout, which is what the muxer and the HDR10 signalling both
// declare. P010 carries its ten bits in the high end of each 16-bit sample.
inline void StoreLuma(unsigned char* plane, int stride, int x, int y, float luma, bool tenBit)
{
    if (tenBit)
    {
        const int v = Clamp((int)(64.0f + 876.0f * luma + 0.5f), 64, 940);
        ((unsigned short*)(plane + (size_t)y * stride))[x] = (unsigned short)(v << 6);
    }
    else
    {
        const int v = Clamp((int)(16.0f + 219.0f * luma + 0.5f), 16, 235);
        plane[(size_t)y * stride + x] = (unsigned char)v;
    }
}

inline void StoreChroma(unsigned char* plane, int stride, int cx, int cy,
                        float cb, float cr, bool tenBit)
{
    if (tenBit)
    {
        unsigned short* row = (unsigned short*)(plane + (size_t)cy * stride);
        row[cx * 2 + 0] = (unsigned short)(Clamp((int)(512.0f + 896.0f * cb + 0.5f), 64, 960) << 6);
        row[cx * 2 + 1] = (unsigned short)(Clamp((int)(512.0f + 896.0f * cr + 0.5f), 64, 960) << 6);
    }
    else
    {
        unsigned char* row = plane + (size_t)cy * stride;
        row[cx * 2 + 0] = (unsigned char)Clamp((int)(128.0f + 224.0f * cb + 0.5f), 16, 240);
        row[cx * 2 + 1] = (unsigned char)Clamp((int)(128.0f + 224.0f * cr + 0.5f), 16, 240);
    }
}

// Rounded up so an odd width still leaves whole chroma pairs on every row. The
// column that padding adds is outside the coded picture but still gets written,
// because handing a driver uninitialised memory is its own class of bug.
inline int LumaStrideBytes(int width, bool tenBit)
{
    return ((width + 1) & ~1) * (tenBit ? 2 : 1);
}

inline int LumaStrideSamples(int width)
{
    return (width + 1) & ~1;
}

inline size_t PlanarFrameBytes(int width, int height, bool tenBit)
{
    const size_t stride = (size_t)LumaStrideBytes(width, tenBit);
    return stride * (size_t)height + stride * (size_t)((height + 1) / 2);
}

void ConvertFrame(const unsigned char* src, int srcPitch, bool srcTenBit,
                  int width, int height, const YuvCoeffs& k,
                  bool dstTenBit, unsigned char* dst)
{
    const int stride = LumaStrideBytes(width, dstTenBit);
    unsigned char* chroma = dst + (size_t)stride * height;

    const float cbScale = 1.0f / (2.0f * (1.0f - k.kb));
    const float crScale = 1.0f / (2.0f * (1.0f - k.kr));

    for (int y = 0; y < height; y += 2)
    {
        // An odd last row or column samples itself twice rather than reading past
        // the end of the frame.
        const int yb = (y + 1 < height) ? (y + 1) : y;
        const unsigned char* rowTop = src + (size_t)y  * srcPitch;
        const unsigned char* rowBot = src + (size_t)yb * srcPitch;

        for (int x = 0; x < width; x += 2)
        {
            const int xr = (x + 1 < width) ? (x + 1) : x;

            const Rgb tl = Unpack(rowTop, x,  srcTenBit);
            const Rgb tr = Unpack(rowTop, xr, srcTenBit);
            const Rgb bl = Unpack(rowBot, x,  srcTenBit);
            const Rgb br = Unpack(rowBot, xr, srcTenBit);

            StoreLuma(dst, stride, x, y, Luma(tl, k), dstTenBit);
            if (xr != x)
                StoreLuma(dst, stride, xr, y, Luma(tr, k), dstTenBit);
            if (yb != y)
            {
                StoreLuma(dst, stride, x, yb, Luma(bl, k), dstTenBit);
                if (xr != x)
                    StoreLuma(dst, stride, xr, yb, Luma(br, k), dstTenBit);
            }

            // Cb and Cr are linear in R, G and B, so averaging the 2x2 block first
            // and converting once gives exactly the same answer as converting four
            // times and averaging, for a quarter of the work.
            const float r = 0.25f * (tl.r + tr.r + bl.r + br.r);
            const float g = 0.25f * (tl.g + tr.g + bl.g + br.g);
            const float b = 0.25f * (tl.b + tr.b + bl.b + br.b);
            const float luma = k.kr * r + k.kg * g + k.kb * b;

            StoreChroma(chroma, stride, x / 2, y / 2,
                        (b - luma) * cbScale, (r - luma) * crScale, dstTenBit);
        }

        if (width & 1)
        {
            // The pad column carries a copy of the edge rather than whatever was in
            // the allocation.
            StoreLuma(dst, stride, width, y, Luma(Unpack(rowTop, width - 1, srcTenBit), k), dstTenBit);
            if (yb != y)
                StoreLuma(dst, stride, width, yb, Luma(Unpack(rowBot, width - 1, srcTenBit), k), dstTenBit);
        }
    }
}

void FillBlack(unsigned char* dst, int width, int height, bool tenBit)
{
    const int stride = LumaStrideBytes(width, tenBit);
    const int chromaRows = (height + 1) / 2;

    if (tenBit)
    {
        const int samples = LumaStrideSamples(width);
        for (int y = 0; y < height; y++)
        {
            unsigned short* row = (unsigned short*)(dst + (size_t)y * stride);
            for (int x = 0; x < samples; x++)
                row[x] = (unsigned short)(64 << 6);
        }
        unsigned short* chroma = (unsigned short*)(dst + (size_t)stride * height);
        const size_t count = (size_t)stride * chromaRows / 2;
        for (size_t i = 0; i < count; i++)
            chroma[i] = (unsigned short)(512 << 6);
    }
    else
    {
        memset(dst, 16, (size_t)stride * height);
        memset(dst + (size_t)stride * height, 128, (size_t)stride * chromaRows);
    }
}

// Scans an Annex-B access unit and copies out the parameter sets, start codes and
// all. HEVC NAL headers are two bytes with the type in bits 1..6 of the first.
bool IsStartCode(const unsigned char* d, size_t size, size_t i, size_t* codeLen)
{
    if (i + 2 >= size || d[i] != 0 || d[i + 1] != 0)
        return false;
    if (d[i + 2] == 1)
    {
        *codeLen = 3;
        return true;
    }
    if (d[i + 2] == 0 && i + 3 < size && d[i + 3] == 1)
    {
        *codeLen = 4;
        return true;
    }
    return false;
}

void ForEachNal(const unsigned char* d, size_t size,
                void (*visit)(void*, const unsigned char*, size_t, unsigned), void* ctx)
{
    size_t i = 0;
    while (i < size)
    {
        size_t codeLen = 0;
        if (!IsStartCode(d, size, i, &codeLen))
        {
            i++;
            continue;
        }

        const size_t nalStart = i + codeLen;
        if (nalStart >= size)
            break;

        size_t j = nalStart;
        size_t nextLen = 0;
        while (j < size && !IsStartCode(d, size, j, &nextLen))
            j++;

        visit(ctx, d + i, j - i, (unsigned)((d[nalStart] >> 1) & 0x3F));
        i = j;
    }
}

void CollectParameterSet(void* ctx, const unsigned char* nal, size_t len, unsigned type)
{
    // 32 is VPS, 33 SPS, 34 PPS.
    if (type != 32 && type != 33 && type != 34)
        return;
    std::vector<unsigned char>* out = (std::vector<unsigned char>*)ctx;
    out->insert(out->end(), nal, nal + len);
}

void NoteIrap(void* ctx, const unsigned char*, size_t, unsigned type)
{
    // 16 through 23 are the random-access picture types, IDR among them.
    if (type >= 16 && type <= 23)
        *(bool*)ctx = true;
}

bool ContainsIrap(const unsigned char* d, size_t size)
{
    bool found = false;
    ForEachNal(d, size, NoteIrap, &found);
    return found;
}

// Media Foundation encoder MFTs will not take an output type with no target
// bitrate, even when the rate controller is then told to chase quality instead, so
// a constant-quality render still needs a number here. A tenth of a bit per pixel
// is a sane HEVC operating point and acts as a ceiling rather than a target.
UINT32 TargetBitrateBps(const EncoderInitParams& p)
{
    if (p.bitrateKbps > 0)
    {
        const int kbps = (p.bitrateKbps > 200000) ? 200000 : p.bitrateKbps;
        return (UINT32)kbps * 1000u;
    }

    double bps = (double)p.width * (double)p.height *
                 (double)p.fpsNum / (double)p.fpsDen * 0.10;
    if (bps < 1000000.0)   bps = 1000000.0;
    if (bps > 200000000.0) bps = 200000000.0;
    return (UINT32)bps;
}

// The job's quality number is a constant-quality level in the codec's own units,
// lower being better, which is the scale NVENC's targetQuality uses. Media
// Foundation wants 0 to 100 with higher better, so the usual 0 to 51 range inverts.
UINT32 QualityToMfScale(int quality)
{
    const int q = Clamp(quality, 0, 51);
    return (UINT32)(100 - (q * 100 / 51));
}

// An IDR every two seconds, the cadence NVENC uses, so seeking behaves the same
// whichever backend produced the file.
UINT32 GopLength(const EncoderInitParams& p)
{
    if (p.fpsNum <= 0 || p.fpsDen <= 0)
        return 120;
    const int gop = p.fpsNum * 2 / p.fpsDen;
    return (gop > 0) ? (UINT32)gop : 120;
}

LONGLONG FrameIndexToHns(long long frameIndex, int fpsNum, int fpsDen)
{
    if (fpsNum <= 0)
        return 0;
    return (LONGLONG)frameIndex * fpsDen * kHnsPerSecond / fpsNum;
}

long long HnsToFrameIndex(LONGLONG hns, int fpsNum, int fpsDen)
{
    const LONGLONG denom = (LONGLONG)fpsDen * kHnsPerSecond;
    if (denom <= 0 || hns <= 0)
        return 0;
    return (long long)((hns * fpsNum + denom / 2) / denom);
}

void ApplyColourAttributes(IMFMediaType* type, const EncoderInitParams& p)
{
    type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);

    if (p.pq2020)
    {
        // The pixels arrived already converted to BT.2020 with the PQ curve; this
        // is only the signalling that tells a display to treat them so.
        type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT2020);
        type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_2084);
        type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT2020_10);
        type->SetUINT32(MF_MT_MAX_LUMINANCE_LEVEL, (UINT32)p.peakNits);
        type->SetUINT32(MF_MT_MAX_MASTERING_LUMINANCE, (UINT32)p.peakNits);
    }
    else
    {
        type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    }
}

HRESULT EnumHevcEncoders(UINT32 flags, IMFActivate*** out, UINT32* count)
{
    MFT_REGISTER_TYPE_INFO outputInfo;
    outputInfo.guidMajorType = MFMediaType_Video;
    outputInfo.guidSubtype   = MFVideoFormat_HEVC;
    return MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, NULL, &outputInfo, out, count);
}

// Hardware first: AMD and Intel both register an async hardware MFT, and a
// software HEVC encoder is slow enough to be a last resort rather than a choice.
const UINT32 kHardwareFlags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;
const UINT32 kSoftwareFlags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT |
                              MFT_ENUM_FLAG_SORTANDFILTER;

const wchar_t kNoEncoderMessage[] =
    L"Windows has no HEVC video encoder registered on this machine. "
    L"One normally arrives with the graphics driver, so updating the driver is "
    L"the usual fix.";

// Hardware entries first, so a caller walking the list is already in preference
// order. Ownership of every activate object moves to 'out'.
void CollectCandidates(std::vector<IMFActivate*>& out)
{
    for (int pass = 0; pass < 2; pass++)
    {
        IMFActivate** acts = NULL;
        UINT32 count = 0;
        if (FAILED(EnumHevcEncoders(pass == 0 ? kHardwareFlags : kSoftwareFlags, &acts, &count))
            || !acts)
            continue;

        for (UINT32 i = 0; i < count; i++)
            out.push_back(acts[i]);
        CoTaskMemFree(acts);
    }
}

void ReleaseCandidates(std::vector<IMFActivate*>& v)
{
    for (size_t i = 0; i < v.size(); i++)
        v[i]->Release();
    v.clear();
}

std::wstring FriendlyName(IMFActivate* act)
{
    std::wstring name;
    WCHAR* nm = NULL;
    UINT32 len = 0;
    if (SUCCEEDED(act->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &nm, &len)) && nm)
    {
        name.assign(nm);
        CoTaskMemFree(nm);
    }
    return name;
}

// AMD's encoder will not negotiate a type until it has been handed a device to
// encode on, and refuses the null manager that means "system memory please". This
// is only built when an encoder turns one down without it.
IMFDXGIDeviceManager* CreateDeviceManager()
{
    ID3D11Device* device = NULL;
    ID3D11DeviceContext* context = NULL;
    D3D_FEATURE_LEVEL level;

    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                                 D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                 NULL, 0, D3D11_SDK_VERSION,
                                 &device, &level, &context)))
        return NULL;
    if (context)
        context->Release();

    // The encoder runs the device from its own threads.
    ID3D10Multithread* mt = NULL;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&mt))) && mt)
    {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }

    UINT token = 0;
    IMFDXGIDeviceManager* manager = NULL;
    if (SUCCEEDED(MFCreateDXGIDeviceManager(&token, &manager)) && manager)
    {
        if (FAILED(manager->ResetDevice(device, token)))
        {
            manager->Release();
            manager = NULL;
        }
    }

    device->Release();
    return manager;
}

// GetEvent with no timeout hangs the whole render if a driver wedges, so poll and
// give up loudly instead. Events are normally already queued, so the first pass
// usually returns without spinning at all.
bool WaitForEvent(IMFMediaEventGenerator* events, MediaEventType* type, std::wstring& error)
{
    const ULONGLONG deadline = GetTickCount64() + kEventTimeoutMs;
    unsigned spins = 0;

    for (;;)
    {
        IMFMediaEvent* ev = NULL;
        const HRESULT hr = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
        if (SUCCEEDED(hr) && ev)
        {
            *type = MEUnknown;
            ev->GetType(type);
            ev->Release();
            return true;
        }
        if (hr != MF_E_NO_EVENTS_AVAILABLE)
        {
            error = Hr(L"The video encoder stopped responding", hr);
            return false;
        }
        if (GetTickCount64() > deadline)
        {
            error = L"The video encoder stopped responding and the render was abandoned.";
            return false;
        }
        Sleep((++spins > 2000) ? 1 : 0);
    }
}

} // namespace

struct MfEncoder::Impl
{
    bool coInited;
    bool mfStarted;

    IMFActivate*            activate;
    IMFTransform*           mft;
    IMFMediaEventGenerator* events;    // async MFTs only
    IMFDXGIDeviceManager*   deviceManager;
    bool                    async;
    bool                    streaming;
    bool                    mftAllocatesOutput;
    DWORD                   inputStreamId;
    DWORD                   outputStreamId;
    DWORD                   outputSampleBytes;

    EncoderInitParams params;
    bool              tenBitOutput;
    bool              downgraded;
    YuvCoeffs         coeffs;
    size_t            frameBytes;

    // Set while the warm-up frame is running, so its access unit feeds the
    // parameter sets instead of the file.
    bool priming;

    std::vector<unsigned char> sequenceParams;
    std::wstring               name;
    long long                  packetsOut;

    Impl()
        : coInited(false), mfStarted(false), activate(NULL), mft(NULL), events(NULL),
          deviceManager(NULL),
          async(false), streaming(false), mftAllocatesOutput(false),
          inputStreamId(0), outputStreamId(0), outputSampleBytes(0),
          tenBitOutput(true), downgraded(false), frameBytes(0), priming(false),
          name(L"Media Foundation HEVC"), packetsOut(0)
    {
        coeffs = kBt709;
    }
};

MfEncoder::MfEncoder() : m_impl(new Impl()) {}

MfEncoder::~MfEncoder()
{
    Shutdown();
    delete m_impl;
    m_impl = NULL;
}

const wchar_t* MfEncoder::Name() const
{
    return m_impl ? m_impl->name.c_str() : L"Media Foundation HEVC";
}

bool MfEncoder::DowngradedToEightBit() const
{
    return m_impl && m_impl->downgraded;
}

bool MfEncoder::IsAvailable(std::wstring* whyNot)
{
    const HRESULT coHr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    bool ok = false;
    if (SUCCEEDED(MFStartup(MF_VERSION)))
    {
        std::vector<IMFActivate*> candidates;
        CollectCandidates(candidates);
        ok = !candidates.empty();
        ReleaseCandidates(candidates);

        if (!ok && whyNot)
            *whyNot = kNoEncoderMessage;
        MFShutdown();
    }
    else if (whyNot)
    {
        *whyNot = L"Could not start Media Foundation.";
    }

    if (SUCCEEDED(coHr))
        CoUninitialize();
    return ok;
}

void MfEncoder::ReleaseTransform()
{
    Impl& d = *m_impl;

    if (d.mft)
    {
        if (d.streaming)
        {
            d.mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        d.mft->Release();
        d.mft = NULL;
    }
    if (d.events) { d.events->Release(); d.events = NULL; }
    if (d.activate)
    {
        d.activate->ShutdownObject();
        d.activate->Release();
        d.activate = NULL;
    }

    d.streaming      = false;
    d.async          = false;
    d.inputStreamId  = 0;
    d.outputStreamId = 0;
}

bool MfEncoder::AttachDeviceManager()
{
    Impl& d = *m_impl;

    if (!d.deviceManager)
        d.deviceManager = CreateDeviceManager();
    if (!d.deviceManager)
        return false;

    return SUCCEEDED(d.mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                           (ULONG_PTR)d.deviceManager));
}

bool MfEncoder::TryCandidate(IMFActivate* candidate, bool tenBit, std::wstring& error)
{
    Impl& d = *m_impl;

    if (FAILED(candidate->ActivateObject(IID_PPV_ARGS(&d.mft))))
    {
        error = L"An HEVC encoder is registered but would not start. "
                L"The graphics driver may be part way through an update.";
        return false;
    }
    candidate->AddRef();
    d.activate = candidate;

    IMFAttributes* attrs = NULL;
    if (SUCCEEDED(d.mft->GetAttributes(&attrs)) && attrs)
    {
        UINT32 isAsync = 0;
        attrs->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
        d.async = (isAsync != 0);

        // A hardware MFT stays locked, refusing everything, until the caller says
        // it knows how to drive the asynchronous model.
        if (d.async)
            attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        attrs->Release();
    }

    if (d.async && FAILED(d.mft->QueryInterface(IID_PPV_ARGS(&d.events))))
    {
        error = L"The video encoder claims to be asynchronous but will not report its events.";
        ReleaseTransform();
        return false;
    }

    // Most encoders number their one input and one output zero, and say so by
    // declining to answer at all.
    DWORD inId = 0, outId = 0;
    if (SUCCEEDED(d.mft->GetStreamIDs(1, &inId, 1, &outId)))
    {
        d.inputStreamId  = inId;
        d.outputStreamId = outId;
    }

    // System memory in, system memory out.
    d.mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);

    if (!ConfigureTypes(tenBit, error))
    {
        // Some encoders will not describe themselves at all until they have a
        // device to encode on. Give one to those, but only to those, so the rest
        // stay on the plain system-memory path.
        if (!AttachDeviceManager() || !ConfigureTypes(tenBit, error))
        {
            ReleaseTransform();
            return false;
        }
    }

    d.name = L"Media Foundation HEVC";
    const std::wstring friendly = FriendlyName(candidate);
    if (!friendly.empty())
        d.name += L" (" + friendly + L")";
    return true;
}

bool MfEncoder::SelectTransform(std::wstring& error)
{
    Impl& d = *m_impl;

    std::vector<IMFActivate*> candidates;
    CollectCandidates(candidates);
    if (candidates.empty())
    {
        error = kNoEncoderMessage;
        return false;
    }

    // Main10 is asked of every encoder on the machine before 8-bit is asked of any,
    // so a hardware encoder that only does 8-bit does not quietly win over a slower
    // one that gives the ten bits that were requested.
    std::wstring tenBitError;
    bool ok = false;

    for (size_t i = 0; i < candidates.size() && !ok; i++)
        ok = TryCandidate(candidates[i], true, tenBitError);

    if (!ok)
    {
        for (size_t i = 0; i < candidates.size() && !ok; i++)
            ok = TryCandidate(candidates[i], false, error);

        if (ok)
        {
            d.downgraded = true;
            d.name += L", 8-bit only";
        }
    }

    ReleaseCandidates(candidates);

    if (!ok)
    {
        error = L"No HEVC encoder on this machine would take this render. " + error +
                L" Ten-bit was refused too: " + tenBitError;
        return false;
    }
    return true;
}

bool MfEncoder::ConfigureTypes(bool tenBit, std::wstring& error)
{
    Impl& d = *m_impl;

    // Encoder MFTs negotiate output first: which input formats they offer depends
    // on the profile that was picked.
    IMFMediaType* outType = NULL;
    if (FAILED(MFCreateMediaType(&outType)))
    {
        error = L"Could not create the HEVC output media type.";
        return false;
    }

    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_HEVC);
    outType->SetUINT32(MF_MT_VIDEO_PROFILE,
                       tenBit ? (UINT32)eAVEncH265VProfile_Main_420_10
                              : (UINT32)eAVEncH265VProfile_Main_420_8);
    outType->SetUINT32(MF_MT_AVG_BITRATE, TargetBitrateBps(d.params));
    MFSetAttributeSize(outType, MF_MT_FRAME_SIZE, d.params.width, d.params.height);
    MFSetAttributeRatio(outType, MF_MT_FRAME_RATE, d.params.fpsNum, d.params.fpsDen);
    MFSetAttributeRatio(outType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, FALSE);
    ApplyColourAttributes(outType, d.params);

    HRESULT hr = d.mft->SetOutputType(d.outputStreamId, outType, 0);
    outType->Release();
    if (FAILED(hr))
    {
        error = RejectedFormat(tenBit ? L"produce 10-bit HEVC" : L"produce HEVC",
                               d.params.width, d.params.height, hr);
        return false;
    }

    ConfigureRateControl();

    const int stride = LumaStrideBytes(d.params.width, tenBit);
    const size_t bytes = PlanarFrameBytes(d.params.width, d.params.height, tenBit);

    IMFMediaType* inType = NULL;
    if (FAILED(MFCreateMediaType(&inType)))
    {
        error = L"Could not create the encoder input media type.";
        return false;
    }

    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, tenBit ? MFVideoFormat_P010 : MFVideoFormat_NV12);
    MFSetAttributeSize(inType, MF_MT_FRAME_SIZE, d.params.width, d.params.height);
    MFSetAttributeRatio(inType, MF_MT_FRAME_RATE, d.params.fpsNum, d.params.fpsDen);
    MFSetAttributeRatio(inType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    inType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    inType->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)stride);
    inType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    inType->SetUINT32(MF_MT_SAMPLE_SIZE, (UINT32)bytes);
    ApplyColourAttributes(inType, d.params);

    hr = d.mft->SetInputType(d.inputStreamId, inType, 0);
    inType->Release();
    if (FAILED(hr))
    {
        error = RejectedFormat(tenBit ? L"take 10-bit P010 frames" : L"take NV12 frames",
                               d.params.width, d.params.height, hr);
        return false;
    }

    d.tenBitOutput = tenBit;
    d.frameBytes   = bytes;
    d.coeffs       = d.params.pq2020 ? kBt2020 : kBt709;
    return true;
}

void MfEncoder::ConfigureRateControl()
{
    Impl& d = *m_impl;

    ICodecAPI* codec = NULL;
    if (FAILED(d.mft->QueryInterface(IID_PPV_ARGS(&codec))) || !codec)
        return;

    VARIANT v;
    memset(&v, 0, sizeof(v));
    v.vt = VT_UI4;

    if (d.params.bitrateKbps > 0)
    {
        v.ulVal = eAVEncCommonRateControlMode_UnconstrainedVBR;
        codec->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
        v.ulVal = TargetBitrateBps(d.params);
        codec->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);
    }
    else
    {
        v.ulVal = eAVEncCommonRateControlMode_Quality;
        codec->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
        v.ulVal = QualityToMfScale(d.params.quality);
        codec->SetValue(&CODECAPI_AVEncCommonQuality, &v);
    }

    // Decode order has to stay display order: the muxer times every sample from its
    // frame index and writes no composition offsets.
    v.ulVal = 0;
    codec->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &v);

    v.ulVal = GopLength(d.params);
    codec->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);

    codec->Release();
}

bool MfEncoder::BeginStreaming(std::wstring& error)
{
    Impl& d = *m_impl;

    MFT_OUTPUT_STREAM_INFO info;
    memset(&info, 0, sizeof(info));
    if (SUCCEEDED(d.mft->GetOutputStreamInfo(d.outputStreamId, &info)))
    {
        d.mftAllocatesOutput = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
        d.outputSampleBytes  = info.cbSize;
    }
    if (d.outputSampleBytes == 0)
        d.outputSampleBytes = (DWORD)((size_t)d.params.width * d.params.height * 2 + 65536);

    HRESULT hr = d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (SUCCEEDED(hr))
        hr = d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr))
    {
        error = Hr(L"The video encoder would not start", hr);
        return false;
    }

    d.streaming = true;
    return true;
}

bool MfEncoder::RestartStreaming(std::wstring& error)
{
    Impl& d = *m_impl;

    d.mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);

    HRESULT hr = d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (SUCCEEDED(hr))
        hr = d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr))
    {
        error = Hr(L"The video encoder would not restart after its warm-up frame", hr);
        return false;
    }

    d.streaming = true;
    return true;
}

bool MfEncoder::HarvestSequenceParams(std::wstring& error)
{
    Impl& d = *m_impl;

    IMFMediaType* current = NULL;
    if (SUCCEEDED(d.mft->GetOutputCurrentType(d.outputStreamId, &current)) && current)
    {
        UINT32 size = 0;
        if (SUCCEEDED(current->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size > 0)
        {
            d.sequenceParams.resize(size);
            if (FAILED(current->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, &d.sequenceParams[0],
                                        size, NULL)))
                d.sequenceParams.clear();
        }
        current->Release();
    }
    if (!d.sequenceParams.empty())
        return true;

    // Most encoders will not describe their output until they have coded something,
    // and the muxer needs VPS/SPS/PPS before the first frame is written. So run one
    // black frame through, keep its parameter sets, and reset the encoder so the
    // frame itself never reaches the file.
    IMFSample* black = NULL;
    if (!BuildBlackSample(&black, error))
        return false;

    d.priming = true;
    const bool ok = SubmitSample(black, error) && DrainToEndOfStream(error);
    d.priming = false;
    black->Release();

    if (!ok)
        return false;
    if (!RestartStreaming(error))
        return false;

    if (d.sequenceParams.empty())
    {
        error = L"The video encoder would not hand over its HEVC headers, "
                L"so the file could not be started.";
        return false;
    }
    return true;
}

bool MfEncoder::Init(const EncoderInitParams& params, std::wstring& error)
{
    Shutdown();
    Impl& d = *m_impl;
    d.params = params;

    if (params.width <= 0 || params.height <= 0)
    {
        error = L"Output dimensions must be positive.";
        return false;
    }
    if (params.fpsNum <= 0 || params.fpsDen <= 0)
    {
        error = L"Frame rate must be positive.";
        return false;
    }

    const HRESULT coHr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    d.coInited = SUCCEEDED(coHr);

    if (FAILED(MFStartup(MF_VERSION)))
    {
        error = L"Could not start Media Foundation.";
        Shutdown();
        return false;
    }
    d.mfStarted = true;

    if (!SelectTransform(error) || !BeginStreaming(error) || !HarvestSequenceParams(error))
    {
        Shutdown();
        return false;
    }
    return true;
}

void MfEncoder::Shutdown()
{
    if (!m_impl)
        return;
    Impl& d = *m_impl;

    ReleaseTransform();
    if (d.deviceManager) { d.deviceManager->Release(); d.deviceManager = NULL; }

    d.mftAllocatesOutput = false;
    d.outputSampleBytes  = 0;
    d.tenBitOutput       = true;
    d.downgraded         = false;
    d.frameBytes         = 0;
    d.priming            = false;
    d.packetsOut         = 0;
    d.name               = L"Media Foundation HEVC";
    d.sequenceParams.clear();

    if (d.mfStarted) { MFShutdown();    d.mfStarted = false; }
    if (d.coInited)  { CoUninitialize(); d.coInited = false; }
}

bool MfEncoder::BuildInputSample(const void* bits, int pitchBytes, long long frameIndex,
                                 IMFSample** out, std::wstring& error)
{
    Impl& d = *m_impl;
    *out = NULL;

    IMFMediaBuffer* buffer = NULL;
    if (FAILED(MFCreateAlignedMemoryBuffer((DWORD)d.frameBytes, MF_16_BYTE_ALIGNMENT, &buffer)))
    {
        error = L"Out of memory while preparing a frame for the encoder.";
        return false;
    }

    BYTE* dst = NULL;
    DWORD maxLen = 0;
    if (FAILED(buffer->Lock(&dst, &maxLen, NULL)))
    {
        buffer->Release();
        error = L"Could not stage a frame for the encoder.";
        return false;
    }

    ConvertFrame((const unsigned char*)bits, pitchBytes, d.params.tenBitInput,
                 d.params.width, d.params.height, d.coeffs, d.tenBitOutput, dst);

    buffer->Unlock();
    buffer->SetCurrentLength((DWORD)d.frameBytes);

    IMFSample* sample = NULL;
    if (FAILED(MFCreateSample(&sample)))
    {
        buffer->Release();
        error = L"Could not create a frame for the encoder.";
        return false;
    }
    sample->AddBuffer(buffer);
    buffer->Release();

    sample->SetSampleTime(FrameIndexToHns(frameIndex, d.params.fpsNum, d.params.fpsDen));
    sample->SetSampleDuration(kHnsPerSecond * d.params.fpsDen / d.params.fpsNum);

    *out = sample;
    return true;
}

bool MfEncoder::BuildBlackSample(IMFSample** out, std::wstring& error)
{
    Impl& d = *m_impl;
    *out = NULL;

    IMFMediaBuffer* buffer = NULL;
    if (FAILED(MFCreateAlignedMemoryBuffer((DWORD)d.frameBytes, MF_16_BYTE_ALIGNMENT, &buffer)))
    {
        error = L"Out of memory while starting the encoder.";
        return false;
    }

    BYTE* dst = NULL;
    DWORD maxLen = 0;
    if (FAILED(buffer->Lock(&dst, &maxLen, NULL)))
    {
        buffer->Release();
        error = L"Could not stage the encoder's warm-up frame.";
        return false;
    }
    FillBlack(dst, d.params.width, d.params.height, d.tenBitOutput);
    buffer->Unlock();
    buffer->SetCurrentLength((DWORD)d.frameBytes);

    IMFSample* sample = NULL;
    if (FAILED(MFCreateSample(&sample)))
    {
        buffer->Release();
        error = L"Could not create the encoder's warm-up frame.";
        return false;
    }
    sample->AddBuffer(buffer);
    buffer->Release();
    sample->SetSampleTime(0);
    sample->SetSampleDuration(kHnsPerSecond * d.params.fpsDen / d.params.fpsNum);

    *out = sample;
    return true;
}

bool MfEncoder::DeliverSample(IMFSample* sample, std::wstring& error)
{
    Impl& d = *m_impl;

    IMFMediaBuffer* buffer = NULL;
    HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(hr) || !buffer)
    {
        error = Hr(L"Could not read a coded frame out of the encoder", hr);
        return false;
    }

    BYTE* data = NULL;
    DWORD maxLen = 0, len = 0;
    hr = buffer->Lock(&data, &maxLen, &len);
    if (FAILED(hr))
    {
        buffer->Release();
        error = Hr(L"Could not read a coded frame out of the encoder", hr);
        return false;
    }

    bool ok = true;
    if (d.priming)
    {
        if (d.sequenceParams.empty())
            ForEachNal(data, len, CollectParameterSet, &d.sequenceParams);
    }
    else if (len > 0)
    {
        // The frame index went out on the sample time and comes back the same way.
        // An encoder that drops the timestamp leaves submission order as the only
        // thing to go on, which is right as long as there are no B-frames, and the
        // rate control setup asks for none.
        LONGLONG ts = 0;
        long long index = d.packetsOut;
        if (SUCCEEDED(sample->GetSampleTime(&ts)))
            index = HnsToFrameIndex(ts, d.params.fpsNum, d.params.fpsDen);
        d.packetsOut++;

        UINT32 cleanPoint = 0;
        const bool key = SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint))
                       ? (cleanPoint != 0)
                       : ContainsIrap(data, len);

        if (d.params.sink)
        {
            EncodedPacket pkt;
            pkt.data       = data;
            pkt.size       = len;
            pkt.frameIndex = index;
            pkt.keyframe   = key;
            ok = d.params.sink(d.params.sinkCtx, pkt, error);
        }
    }

    buffer->Unlock();
    buffer->Release();
    return ok;
}

bool MfEncoder::PullOutput(bool* got, std::wstring& error)
{
    Impl& d = *m_impl;
    *got = false;

    MFT_OUTPUT_DATA_BUFFER out;
    memset(&out, 0, sizeof(out));
    out.dwStreamID = d.outputStreamId;

    if (!d.mftAllocatesOutput)
    {
        IMFMediaBuffer* buffer = NULL;
        if (FAILED(MFCreateAlignedMemoryBuffer(d.outputSampleBytes, MF_16_BYTE_ALIGNMENT, &buffer)))
        {
            error = L"Out of memory while collecting a coded frame.";
            return false;
        }
        if (FAILED(MFCreateSample(&out.pSample)))
        {
            buffer->Release();
            error = L"Could not create a buffer for a coded frame.";
            return false;
        }
        out.pSample->AddBuffer(buffer);
        buffer->Release();
    }

    DWORD status = 0;
    const HRESULT hr = d.mft->ProcessOutput(0, 1, &out, &status);

    if (out.pEvents)
    {
        out.pEvents->Release();
        out.pEvents = NULL;
    }

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
    {
        if (out.pSample) out.pSample->Release();
        return true;
    }

    if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
    {
        // The encoder revised its own output type. Nothing here depends on the
        // details, but the parameter sets do, so pick them up again.
        if (out.pSample) out.pSample->Release();

        IMFMediaType* revised = NULL;
        if (SUCCEEDED(d.mft->GetOutputAvailableType(d.outputStreamId, 0, &revised)) && revised)
        {
            const HRESULT setHr = d.mft->SetOutputType(d.outputStreamId, revised, 0);
            revised->Release();
            if (FAILED(setHr))
            {
                error = Hr(L"The video encoder changed its output format partway through "
                           L"and the new one was refused", setHr);
                return false;
            }
        }
        d.sequenceParams.clear();
        IMFMediaType* current = NULL;
        if (SUCCEEDED(d.mft->GetOutputCurrentType(d.outputStreamId, &current)) && current)
        {
            UINT32 size = 0;
            if (SUCCEEDED(current->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size > 0)
            {
                d.sequenceParams.resize(size);
                if (FAILED(current->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER,
                                            &d.sequenceParams[0], size, NULL)))
                    d.sequenceParams.clear();
            }
            current->Release();
        }
        return true;
    }

    if (FAILED(hr))
    {
        if (out.pSample) out.pSample->Release();
        error = Hr(L"The video encoder failed partway through a frame", hr);
        return false;
    }

    if (!out.pSample)
        return true;

    const bool ok = DeliverSample(out.pSample, error);
    out.pSample->Release();
    *got = ok;
    return ok;
}

bool MfEncoder::DrainPending(std::wstring& error)
{
    for (;;)
    {
        bool got = false;
        if (!PullOutput(&got, error))
            return false;
        if (!got)
            return true;
    }
}

bool MfEncoder::SubmitSample(IMFSample* sample, std::wstring& error)
{
    Impl& d = *m_impl;

    if (d.async)
    {
        // An async MFT asks for input rather than being told, so service whatever
        // output it has queued until it does.
        for (;;)
        {
            MediaEventType type = MEUnknown;
            if (!WaitForEvent(d.events, &type, error))
                return false;

            if (type == METransformNeedInput)
            {
                const HRESULT hr = d.mft->ProcessInput(d.inputStreamId, sample, 0);
                if (FAILED(hr))
                {
                    error = Hr(L"The video encoder refused a frame", hr);
                    return false;
                }
                return true;
            }
            if (type == METransformHaveOutput)
            {
                bool got = false;
                if (!PullOutput(&got, error))
                    return false;
            }
        }
    }

    HRESULT hr = d.mft->ProcessInput(d.inputStreamId, sample, 0);
    if (hr == MF_E_NOTACCEPTING)
    {
        if (!DrainPending(error))
            return false;
        hr = d.mft->ProcessInput(d.inputStreamId, sample, 0);
    }
    if (FAILED(hr))
    {
        error = Hr(L"The video encoder refused a frame", hr);
        return false;
    }
    return DrainPending(error);
}

bool MfEncoder::DrainToEndOfStream(std::wstring& error)
{
    Impl& d = *m_impl;

    d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    const HRESULT hr = d.mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    if (FAILED(hr))
    {
        error = Hr(L"The video encoder would not finish the frames it still held", hr);
        return false;
    }

    if (!d.async)
        return DrainPending(error);

    for (;;)
    {
        MediaEventType type = MEUnknown;
        if (!WaitForEvent(d.events, &type, error))
            return false;

        if (type == METransformHaveOutput)
        {
            bool got = false;
            if (!PullOutput(&got, error))
                return false;
        }
        else if (type == METransformDrainComplete)
        {
            return true;
        }
        // A request for input after a drain has started is to be ignored.
    }
}

bool MfEncoder::EncodeFrame(const void* bits, int pitchBytes,
                            long long frameIndex, std::wstring& error)
{
    Impl& d = *m_impl;
    if (!d.mft || !d.streaming)
    {
        error = L"The encoder is not running.";
        return false;
    }
    if (!bits || pitchBytes < d.params.width * 4)
    {
        error = L"The frame handed to the encoder was not the size it was told to expect.";
        return false;
    }

    IMFSample* sample = NULL;
    if (!BuildInputSample(bits, pitchBytes, frameIndex, &sample, error))
        return false;

    const bool ok = SubmitSample(sample, error);
    sample->Release();
    return ok;
}

bool MfEncoder::Flush(std::wstring& error)
{
    Impl& d = *m_impl;
    if (!d.mft || !d.streaming)
        return true;

    if (!DrainToEndOfStream(error))
        return false;

    d.streaming = false;
    return true;
}

bool MfEncoder::GetSequenceParams(std::vector<unsigned char>& out)
{
    if (!m_impl || m_impl->sequenceParams.empty())
        return false;
    out = m_impl->sequenceParams;
    return true;
}

} // namespace offline
