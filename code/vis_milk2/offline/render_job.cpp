#include "render_job.h"

#include <d3d9.h>
#include <stdlib.h>
#include <string>
#include <math.h>

#include "../plugin.h"
#include "../app_device.h"
#include "frame_grabber.h"
#include "audio_source.h"
#include "nvenc_encoder.h"
#include "mf_encoder.h"
#include "mp4_muxer.h"
#include "pq_post.h"
#include "render_log.h"

extern CPlugin g_plugin;

namespace offline {

namespace {

const wchar_t kOfflineWindowClass[] = L"MilkRunOfflineRenderWindow";

// MilkDrop's shell handles most messages itself; the offline window exists only to
// give D3D9 something to hang a swap chain on, so nothing here needs interpreting.
LRESULT CALLBACK OfflineWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

// Never shown. m_hidden in the shell is only set by WM_SIZE with SIZE_MINIMIZED or
// SIZE_MAXHIDE, and a window that is never shown receives neither, so the render
// loop does not get short-circuited by the hidden-window early-out in PluginRender.
HWND CreateOfflineWindow(HINSTANCE hInstance, int width, int height)
{
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc   = OfflineWndProc;
    wc.hInstance     = hInstance;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = kOfflineWindowClass;

    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return NULL;

    return CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kOfflineWindowClass,
        L"MilkRun offline render",
        WS_POPUP,
        0, 0, width, height,
        NULL, NULL, hInstance, NULL);
}

// Drain anything the window or D3D posted so nothing accumulates over a long render.
void PumpMessages()
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

double NowSeconds()
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

// Bridges the encoder's packet callback to the muxer.
struct EncodeSinkCtx
{
    IMuxer*      muxer;
    long long    packets;
};

bool OnEncodedPacket(void* ctx, const EncodedPacket& pkt, std::wstring& error)
{
    EncodeSinkCtx* c = (EncodeSinkCtx*)ctx;
    if (!c->muxer->WriteVideoPacket(pkt, error))
        return false;
    c->packets++;
    return true;
}
RenderResult Fail(const wchar_t* msg)
{
    RenderResult r;
    r.ok = false;
    r.error = msg;
    return r;
}

} // namespace

RenderResult RunRenderJob(HINSTANCE hInstance,
                          const RenderJobConfig& cfg,
                          ProgressFn onProgress,
                          void* progressCtx)
{
    RenderLog log;
    log.Begin(cfg.outputPath, cfg);

    // Validation reports through the log as well as the return value. A job
    // rejected before it starts is the one most in need of a written record,
    // since nothing else about it survives.
    #define FAIL_LOGGED(msg) do { log.End(false, 0, 0.0, (msg), L""); return Fail(msg); } while (0)

    if (cfg.width <= 0 || cfg.height <= 0)
        FAIL_LOGGED(L"Output dimensions must be positive.");
    if (cfg.fpsNum <= 0 || cfg.fpsDen <= 0)
        FAIL_LOGGED(L"Frame rate must be positive.");
    if (cfg.presetPath.empty())
        FAIL_LOGGED(L"No preset selected.");
    if (GetFileAttributesW(cfg.presetPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        FAIL_LOGGED(L"Preset file not found.");

    const float fps = (float)cfg.fpsNum / (float)cfg.fpsDen;

    // The audio decides the length: the video runs for exactly as long as the song,
    // with nothing padded onto either end.
    AudioSource audio;
    bool haveAudio = false;
    if (!cfg.audioPath.empty())
    {
        std::wstring audioError;
        if (!audio.Open(cfg.audioPath, audioError))
        {
            log.End(false, 0, 0.0, audioError, L"");
            return Fail(audioError.c_str());
        }
        haveAudio = true;
        log.RecordAudio(cfg.audioPath.c_str(), audio.DurationSeconds(), AudioSource::kSampleRate, false);
    }

    // Audio frame index for output frame 0, when rendering a section.
    long long audioFrameOffset = 0;
    if (haveAudio && cfg.startSeconds > 0.0)
    {
        audioFrameOffset = (long long)(cfg.startSeconds * cfg.fpsNum / cfg.fpsDen + 0.5);
        if (cfg.startSeconds >= audio.DurationSeconds())
            return Fail(L"The start offset is past the end of the song.");
    }

    long long frameCount = cfg.frameCountOverride;
    if (frameCount <= 0)
    {
        if (!haveAudio)
            return Fail(L"No audio file given, so there is nothing to set the length from.");

        const double span = (cfg.durationSeconds > 0.0)
                          ? cfg.durationSeconds
                          : (audio.DurationSeconds() - cfg.startSeconds);
        frameCount = (long long)ceil(span * cfg.fpsNum / cfg.fpsDen - 1e-9);

        // Never run past the song.
        const long long available = audio.FrameCount(cfg.fpsNum, cfg.fpsDen) - audioFrameOffset;
        if (frameCount > available)
            frameCount = available;
    }
    if (frameCount <= 0)
        return Fail(L"The audio file appears to be empty.");

    // MilkDrop seeds several per-frame effects from rand(), so pin it to make a
    // repeat render of the same job byte-identical.
    srand(cfg.randomSeed);

    HWND hwnd = CreateOfflineWindow(hInstance, cfg.width, cfg.height);
    if (!hwnd)
        return Fail(L"Could not create the offline render window.");

    // Offline mode has to be set before PluginPreInitialize, because the overrides
    // that silence the overlays are applied at the tail of MyReadConfig, which runs
    // inside it and is the last thing to touch those settings.
    g_plugin.SetOfflineMode(true, fps);
    lstrcpynW(g_plugin.m_szOfflinePreset, cfg.presetPath.c_str(), MAX_PATH);

    g_plugin.PluginPreInitialize(0, 0);

    // Internal canvas precision. Read from milk2.ini during MyReadConfig, so this
    // has to land after PluginPreInitialize and before the textures are allocated
    // in PluginInitialize.
    g_plugin.m_nTexBitsPerCh = (cfg.precision == Precision::High) ? 16 : 8;

    const D3DFORMAT backBuf = (cfg.precision == Precision::High)
                            ? D3DFMT_A2R10G10B10
                            : D3DFMT_UNKNOWN;

    RenderResult result;
    if (!InitD3d(hwnd, cfg.width, cfg.height, true, backBuf))
    {
        DestroyWindow(hwnd);
        g_plugin.SetOfflineMode(false, 0.0f);
        log.End(false, 0, 0.0, L"Could not create a Direct3D 9 device for the render.", L"");
        return Fail(L"Could not create a Direct3D 9 device for the render.");
    }

    if (!g_plugin.PluginInitialize(pD3DDevice, &d3dPp, hwnd, cfg.width, cfg.height))
    {
        DeinitD3d();
        DestroyWindow(hwnd);
        g_plugin.SetOfflineMode(false, 0.0f);
        log.End(false, 0, 0.0, L"MilkDrop could not start the render. The most likely cause is a preset that could not be loaded or compiled.", L"");
        return Fail(L"MilkDrop could not start the render. The most likely cause is a preset that could not be loaded or compiled.");
    }

    // HDR10 needs the float intermediate in place before the first frame renders.
    PqPostProcess pqPost;
    if (cfg.pq2020)
    {
        std::wstring pqError;
        if (!pqPost.Init(pD3DDevice, cfg.width, cfg.height,
                         cfg.diffuseWhiteNits, cfg.peakNits, pqError))
        {
            g_plugin.PluginQuit();
            DeinitD3d();
            DestroyWindow(hwnd);
            g_plugin.SetOfflineMode(false, 0.0f);
            return Fail(pqError.c_str());
        }
        g_plugin.m_pPqPost = &pqPost;
    }

    log.RecordPresetInUse(g_plugin.m_szCurrentPresetFile, g_plugin.m_pState->m_szDesc);

    FrameGrabber grabber;
    if (!grabber.Init(pD3DDevice, cfg.width, cfg.height))
    {
        g_plugin.PluginQuit();
        DeinitD3d();
        DestroyWindow(hwnd);
        g_plugin.SetOfflineMode(false, 0.0f);
        log.End(false, 0, 0.0, L"Could not create the frame readback surface.", L"");
        return Fail(L"Could not create the frame readback surface.");
    }

    const bool dumping = !cfg.dumpFramesDir.empty();
    if (dumping)
        CreateDirectoryW(cfg.dumpFramesDir.c_str(), NULL);

    // Encoding is optional so the smoke-test path can still run without an output
    // file, but a render that was asked for a file has to produce one.
    // Both are constructed; only the selected one is initialized, and an encoder
    // that was never initialized costs nothing.
    NvencEncoder   nvenc;
    MfEncoder      mfenc;
    IVideoEncoder* encoder = NULL;
    Mp4Muxer      muxer;
    EncodeSinkCtx sinkCtx;
    sinkCtx.muxer   = &muxer;
    sinkCtx.packets = 0;

    const bool encoding = !cfg.outputPath.empty();
    if (encoding)
    {
        EncoderInitParams ep;
        ep.device           = pD3DDevice;
        ep.width            = cfg.width;
        ep.height           = cfg.height;
        ep.fpsNum           = cfg.fpsNum;
        ep.fpsDen           = cfg.fpsDen;
        ep.tenBitInput      = (grabber.Format() == D3DFMT_A2R10G10B10);
        ep.bitrateKbps      = cfg.bitrateKbps;
        ep.quality          = cfg.quality;
        ep.pq2020           = cfg.pq2020;
        ep.diffuseWhiteNits = cfg.diffuseWhiteNits;
        ep.peakNits         = cfg.peakNits;
        ep.sink             = OnEncodedPacket;
        ep.sinkCtx          = &sinkCtx;

        // Auto prefers NVENC and falls back to Media Foundation, which reaches AMD
        // and Intel through their own driver MFTs. An explicit choice is honoured
        // even when the other would work, so a result can always be pinned on the
        // backend that produced it.
        std::wstring encError;
        if (cfg.backend == EncoderBackend::Nvenc)
        {
            encoder = &nvenc;
        }
        else if (cfg.backend == EncoderBackend::MediaFoundation)
        {
            encoder = &mfenc;
        }
        else
        {
            std::wstring why;
            encoder = NvencEncoder::IsAvailable(pD3DDevice, &why)
                        ? (IVideoEncoder*)&nvenc : (IVideoEncoder*)&mfenc;
        }

        // A machine can advertise NVENC and still fail to open a session, so Auto
        // gets one try on the other backend before the render is given up on.
        bool encOk = encoder->Init(ep, encError);
        if (!encOk && cfg.backend == EncoderBackend::Auto && encoder == (IVideoEncoder*)&nvenc)
        {
            encoder = &mfenc;
            encOk = encoder->Init(ep, encError);
        }

        if (!encOk)
        {
            g_plugin.m_pPqPost = NULL;
    pqPost.Shutdown();
    grabber.Shutdown();
            g_plugin.PluginQuit();
            DeinitD3d();
            DestroyWindow(hwnd);
            g_plugin.SetOfflineMode(false, 0.0f);
            return Fail(encError.c_str());
        }

        log.RecordEncoder(encoder->Name(), ep.tenBitInput, cfg.pq2020);

        MuxerInitParams mp;
        mp.outputPath = cfg.outputPath;
        mp.width      = cfg.width;
        mp.height     = cfg.height;
        mp.fpsNum     = cfg.fpsNum;
        mp.fpsDen     = cfg.fpsDen;
        mp.audioPath  = cfg.audioPath;
        mp.startSeconds = cfg.startSeconds;
        mp.totalFrames  = frameCount;
        mp.pq2020     = cfg.pq2020;
        mp.peakNits   = cfg.peakNits;
        encoder->GetSequenceParams(mp.sequenceParams);

        std::wstring muxError;
        if (!muxer.Init(mp, muxError))
        {
            encoder->Shutdown();
            g_plugin.m_pPqPost = NULL;
    pqPost.Shutdown();
    grabber.Shutdown();
            g_plugin.PluginQuit();
            DeinitD3d();
            DestroyWindow(hwnd);
            g_plugin.SetOfflineMode(false, 0.0f);
            return Fail(muxError.c_str());
        }
    }

    // Falls back to silence when a frame count was given with no audio, which is
    // how the smoke-test path runs.
    unsigned char waveL[AudioSource::kWindowSamples] = { 0 };
    unsigned char waveR[AudioSource::kWindowSamples] = { 0 };

    const double started = NowSeconds();
    bool cancelled = false;

    for (long long i = 0; i < frameCount; i++)
    {
        PumpMessages();

        if (haveAudio && !audio.FillFrameWindow(i + audioFrameOffset, cfg.fpsNum, cfg.fpsDen, waveL, waveR))
        {
            result.error = L"Ran out of decodable audio partway through.";
            break;
        }

        if (!g_plugin.PluginRender(waveL, waveR))
        {
            result.error = L"The visualizer stopped rendering partway through.";
            break;
        }

        const void* bits = NULL;
        int pitch = 0;
        if (!grabber.Grab(&bits, &pitch))
        {
            result.error = L"Could not read a frame back from the GPU.";
            break;
        }

        if (encoding)
        {
            std::wstring encError;
            if (!encoder->EncodeFrame(bits, pitch, i, encError))
            {
                grabber.Unlock();
                result.error = encError;
                break;
            }
        }

        grabber.Unlock();

        if (dumping && cfg.dumpFrameStride > 0 && (i % cfg.dumpFrameStride) == 0)
        {
            wchar_t path[MAX_PATH];
            swprintf_s(path, L"%s\\frame_%06lld.png", cfg.dumpFramesDir.c_str(), i);
            grabber.SaveLastFrame(path);
        }

        if (onProgress)
        {
            RenderProgress p;
            p.frameIndex     = i + 1;
            p.frameCount     = frameCount;
            p.elapsedSeconds = NowSeconds() - started;
            if (!onProgress(progressCtx, p))
            {
                cancelled = true;
                break;
            }
        }

        result.framesWritten++;
    }

    const bool renderedCleanly = result.error.empty() && !cancelled;

    // Drain whatever the encoder is still holding, then close the container. Both
    // have to happen before the device goes away, since the encoder session is
    // bound to it.
    if (encoding)
    {
        if (renderedCleanly)
        {
            std::wstring flushError;
            if (!encoder->Flush(flushError))
                result.error = flushError;
        }

        if (encoder == (IVideoEncoder*)&mfenc && mfenc.DowngradedToEightBit())
        {
            result.warning = L"This machine's HEVC encoder would not accept 10-bit "
                             L"frames, so the file is 8-bit. Gradients will band "
                             L"more than a 10-bit render would.";
        }

        encoder->Shutdown();

        if (result.error.empty() && !cancelled)
        {
            std::wstring muxError;
            if (!muxer.Finalize(muxError))
                result.error = muxError;
        }
        else
        {
            // Leaves no half-written file behind.
            muxer.Abort();
        }
    }

    result.elapsedSeconds = NowSeconds() - started;

    log.End(result.error.empty() && !cancelled,
            result.framesWritten, result.elapsedSeconds, result.error, result.warning);

    g_plugin.m_pPqPost = NULL;
    pqPost.Shutdown();
    grabber.Shutdown();

    // Deliberately not calling MyWriteConfig: a render must not rewrite the user's
    // milk2.ini with the offline overrides.
    g_plugin.PluginQuit();
    DeinitD3d();
    DestroyWindow(hwnd);
    g_plugin.SetOfflineMode(false, 0.0f);

    if (cancelled)
    {
        result.ok = false;
        result.error = L"Render cancelled.";
    }
    else if (result.error.empty())
    {
        result.ok = (result.framesWritten == frameCount);
        if (!result.ok)
            result.error = L"Render ended early.";
    }

    return result;
}

} // namespace offline
