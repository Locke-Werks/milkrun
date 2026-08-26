#include "player_window.h"
#include "../resource.h"

#include <windowsx.h>
#include <mfapi.h>
#include <mfobjects.h>
#include <mfmediaengine.h>
#include <mferror.h>
#include <shlwapi.h>
#include <math.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "oleaut32.lib")

namespace offline {

namespace {

// Posted from a media engine worker thread. wParam is the MF_MEDIA_ENGINE_EVENT,
// lParam the event's second parameter, which for the error event is the HRESULT.
const UINT WM_PLAYER_ENGINE_EVENT = WM_APP + 1;

const UINT_PTR kTimerTick = 1;
const UINT     kTickMs    = 33;

// Fullscreen hides the transport once the mouse has been still this long.
const DWORD kIdleHideMs = 2500;

// Transport metrics, in pixels at 96 dpi.
const int kBarHeight = 54;
const int kBarPadX   = 18;
const int kGrooveY   = 15;
const int kGrooveH   = 6;
const int kKnobR     = 7;
const int kTextY     = 30;
const int kGlyphW    = 11;
const int kGlyphH    = 13;

const COLORREF kColBack      = RGB(0x12, 0x12, 0x16);
const COLORREF kColLetterbox = RGB(0x00, 0x00, 0x00);
const COLORREF kColGroove    = RGB(0x2c, 0x2c, 0x36);
const COLORREF kColPlayed    = RGB(0x6e, 0xc1, 0xff);
const COLORREF kColKnob      = RGB(0xff, 0xff, 0xff);
const COLORREF kColText      = RGB(0xc8, 0xc8, 0xd2);
const COLORREF kColHint      = RGB(0x66, 0x66, 0x74);

const wchar_t kPlayerClass[] = L"MilkRunPlayerWindow";
const wchar_t kVideoClass[]  = L"MilkRunPlayerVideo";

bool Finite(double v)
{
    // Duration is infinity for a live source and NaN before metadata arrives.
    return v == v && v > -1e15 && v < 1e15;
}

std::wstring FormatTime(double seconds)
{
    if (!(seconds >= 0.0))   // also catches NaN
        seconds = 0.0;

    const int whole = (int)seconds;
    const int cs    = (int)((seconds - whole) * 100.0);
    const int h     = whole / 3600;
    const int m     = (whole / 60) % 60;
    const int s     = whole % 60;

    wchar_t buf[48];
    if (h > 0)
        swprintf_s(buf, L"%d:%02d:%02d.%02d", h, m, s, cs);
    else
        swprintf_s(buf, L"%d:%02d.%02d", m, s, cs);
    return buf;
}

// The engine reports "cannot decode" several different ways depending on how far
// the pipeline got before it gave up. All of them mean the same thing for a file
// this program wrote, because it only ever writes HEVC.
bool IsMissingDecoder(MF_MEDIA_ENGINE_ERR err, HRESULT ext)
{
    if (ext == MF_E_TOPO_CODEC_NOT_FOUND ||
        ext == MF_E_UNSUPPORTED_FORMAT ||
        ext == MF_E_INVALIDMEDIATYPE)
        return true;

    // A missing decoder often surfaces with no extended code at all.
    return (err == MF_MEDIA_ENGINE_ERR_SRC_NOT_SUPPORTED ||
            err == MF_MEDIA_ENGINE_ERR_DECODE) && ext == 0;
}

// Bridges media engine events onto the window thread.
//
// This has to be a separate object rather than something the player itself
// implements. The engine holds a reference for as long as it lives and calls back
// on its own threads, so tying its lifetime to a window or to a stack object is
// how this crashes. Detach severs the link before the window goes away; the object
// itself lives until both the engine and the player have released it.
class EngineNotify : public IMFMediaEngineNotify
{
public:
    explicit EngineNotify(HWND hwnd) : m_refs(1), m_hwnd(hwnd)
    {
        InitializeCriticalSection(&m_lock);
    }

    // Called on the window thread before the engine is shut down.
    void Detach()
    {
        EnterCriticalSection(&m_lock);
        m_hwnd = NULL;
        LeaveCriticalSection(&m_lock);
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv)
            return E_POINTER;

        if (riid == IID_IUnknown || riid == IID_IMFMediaEngineNotify)
        {
            *ppv = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }

        *ppv = NULL;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef()
    {
        return (ULONG)InterlockedIncrement(&m_refs);
    }

    STDMETHODIMP_(ULONG) Release()
    {
        const LONG left = InterlockedDecrement(&m_refs);
        if (left == 0)
            delete this;
        return (ULONG)left;
    }

    STDMETHODIMP EventNotify(DWORD event, DWORD_PTR /*param1*/, DWORD param2)
    {
        // On a worker thread. Everything worth doing touches the window or the
        // engine, both of which belong to another thread, so hand it over and
        // return without blocking the engine.
        EnterCriticalSection(&m_lock);
        if (m_hwnd)
            PostMessageW(m_hwnd, WM_PLAYER_ENGINE_EVENT, (WPARAM)event, (LPARAM)param2);
        LeaveCriticalSection(&m_lock);
        return S_OK;
    }

private:
    ~EngineNotify()
    {
        DeleteCriticalSection(&m_lock);
    }

    LONG             m_refs;
    HWND             m_hwnd;
    CRITICAL_SECTION m_lock;
};

void FillSolid(HDC dc, const RECT& r, COLORREF color)
{
    const COLORREF prev = SetBkColor(dc, color);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &r, NULL, 0, NULL);
    SetBkColor(dc, prev);
}

void FillRounded(HDC dc, const RECT& r, int radius, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen   = SelectObject(dc, GetStockObject(NULL_PEN));

    // RoundRect excludes the right and bottom edge with a null pen, so pad by one.
    RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, radius * 2, radius * 2);

    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(brush);
}

} // namespace

struct PlayerWindow::Impl
{
    HINSTANCE hinst;
    HWND      hwnd;        // top level: letterbox and transport
    HWND      hwndVideo;   // child the engine paints into

    IMFMediaEngine*   engine;
    IMFMediaEngineEx* engineEx;
    EngineNotify*     notify;

    bool comInited;
    bool mfStarted;
    bool quit;

    PlayerOptions opts;
    std::wstring  path;
    std::wstring  error;

    int  videoW;
    int  videoH;
    bool sizedToVideo;

    double duration;
    double position;    // what the transport shows, which is the scrub position while dragging

    bool scrubbing;
    bool ended;

    bool            fullscreen;
    WINDOWPLACEMENT placement;
    LONG            styleBeforeFullscreen;

    bool  barVisible;
    DWORD lastMouseTick;

    int   dpi;
    HFONT font;

    Impl()
        : hinst(NULL), hwnd(NULL), hwndVideo(NULL),
          engine(NULL), engineEx(NULL), notify(NULL),
          comInited(false), mfStarted(false), quit(false),
          videoW(0), videoH(0), sizedToVideo(false),
          duration(0.0), position(0.0), scrubbing(false), ended(false),
          fullscreen(false), styleBeforeFullscreen(0),
          barVisible(true), lastMouseTick(0), dpi(96), font(NULL)
    {
        ZeroMemory(&placement, sizeof(placement));
        placement.length = sizeof(placement);
    }

    int S(int v) const { return MulDiv(v, dpi, 96); }

    int  BarHeight() const { return barVisible ? S(kBarHeight) : 0; }
    RECT BarRect() const;
    RECT GrooveRect() const;
    RECT SeekHitRect() const;
    RECT GlyphRect() const;

    void Layout();
    void SizeToVideo();
    void Paint(HDC dc, const RECT& client);

    void TogglePlay();
    void StepFrame(int direction);
    void SeekToX(int x, bool commit);
    void ToggleFullscreen();
    void ShowBar(bool show);

    void OnEngineEvent(DWORD event, HRESULT param2);
    void FailWith(const std::wstring& message);
    void ShutdownEngine();

    bool IsPlaying() const { return engine && !engine->IsPaused(); }
    long long FrameAt(double seconds) const;

    static LRESULT CALLBACK PlayerProc(HWND h, UINT msg, WPARAM w, LPARAM l);
    static LRESULT CALLBACK VideoProc(HWND h, UINT msg, WPARAM w, LPARAM l);
};

RECT PlayerWindow::Impl::BarRect() const
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    rc.top = rc.bottom - BarHeight();
    return rc;
}

RECT PlayerWindow::Impl::GrooveRect() const
{
    const RECT bar = BarRect();
    RECT g;
    g.left   = bar.left + S(kBarPadX);
    g.right  = bar.right - S(kBarPadX);
    g.top    = bar.top + S(kGrooveY);
    g.bottom = g.top + S(kGrooveH);
    if (g.right < g.left)
        g.right = g.left;
    return g;
}

// Generous enough that the seek bar can be grabbed without aiming at six pixels.
RECT PlayerWindow::Impl::SeekHitRect() const
{
    const RECT bar = BarRect();
    RECT h = GrooveRect();
    h.left  -= S(kKnobR);
    h.right += S(kKnobR);
    h.top    = bar.top;
    h.bottom = bar.top + S(kGrooveY + kGrooveH + 6);
    return h;
}

RECT PlayerWindow::Impl::GlyphRect() const
{
    const RECT bar = BarRect();
    RECT g;
    g.left   = bar.left + S(kBarPadX);
    g.right  = g.left + S(kGlyphW);
    g.top    = bar.top + S(kTextY) - S(kGlyphH) / 2 + S(6);
    g.bottom = g.top + S(kGlyphH);
    return g;
}

void PlayerWindow::Impl::Layout()
{
    if (!hwnd || !hwndVideo)
        return;

    RECT rc;
    GetClientRect(hwnd, &rc);

    int availW = rc.right;
    int availH = rc.bottom - BarHeight();
    if (availW < 1) availW = 1;
    if (availH < 1) availH = 1;

    const int vw = (videoW > 0) ? videoW : 16;
    const int vh = (videoH > 0) ? videoH : 9;

    int w = availW;
    int h = MulDiv(availW, vh, vw);
    if (h > availH)
    {
        h = availH;
        w = MulDiv(availH, vw, vh);
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    MoveWindow(hwndVideo, (availW - w) / 2, (availH - h) / 2, w, h, TRUE);

    // The engine does not repaint a paused frame on its own when the window moves,
    // so hand it the new rectangle explicitly.
    if (engineEx)
    {
        MFVideoNormalizedRect src = { 0.0f, 0.0f, 1.0f, 1.0f };
        MFARGB border = { 0, 0, 0, 255 };
        RECT dst = { 0, 0, w, h };
        engineEx->UpdateVideoStream(&src, &dst, &border);
    }

    InvalidateRect(hwnd, NULL, FALSE);
}

void PlayerWindow::Impl::SizeToVideo()
{
    if (sizedToVideo || fullscreen || videoW <= 0 || videoH <= 0)
        return;
    sizedToVideo = true;

    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi))
        return;

    const int workW = mi.rcWork.right - mi.rcWork.left;
    const int workH = mi.rcWork.bottom - mi.rcWork.top;

    // Leave room for the window frame and the transport as well as the video.
    RECT frame = { 0, 0, 0, 0 };
    AdjustWindowRect(&frame, (DWORD)GetWindowLongW(hwnd, GWL_STYLE), FALSE);
    const int chromeW = (frame.right - frame.left);
    const int chromeH = (frame.bottom - frame.top) + S(kBarHeight);

    const int maxW = MulDiv(workW, 85, 100) - chromeW;
    const int maxH = MulDiv(workH, 85, 100) - chromeH;
    if (maxW < 160 || maxH < 90)
        return;

    int w = videoW;
    int h = videoH;
    if (w > maxW) { h = MulDiv(h, maxW, w); w = maxW; }
    if (h > maxH) { w = MulDiv(w, maxH, h); h = maxH; }

    const int outerW = w + chromeW;
    const int outerH = h + chromeH;

    SetWindowPos(hwnd, NULL,
                 mi.rcWork.left + (workW - outerW) / 2,
                 mi.rcWork.top  + (workH - outerH) / 2,
                 outerW, outerH,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

long long PlayerWindow::Impl::FrameAt(double seconds) const
{
    if (!(seconds > 0.0) || opts.fpsDen <= 0 || opts.fpsNum <= 0)
        return 0;
    return (long long)floor(seconds * opts.fpsNum / opts.fpsDen + 0.5);
}

void PlayerWindow::Impl::Paint(HDC dc, const RECT& client)
{
    RECT letterbox = client;
    letterbox.bottom -= BarHeight();
    if (letterbox.bottom > letterbox.top)
        FillSolid(dc, letterbox, kColLetterbox);

    if (!barVisible)
        return;

    const RECT bar = BarRect();
    FillSolid(dc, bar, kColBack);

    // --- seek bar ----------------------------------------------------------
    const RECT groove = GrooveRect();
    const int radius = S(kGrooveH) / 2;
    FillRounded(dc, groove, radius, kColGroove);

    const double shown = scrubbing ? position
                                   : (engine ? engine->GetCurrentTime() : 0.0);
    const double fraction = (duration > 0.0 && Finite(shown))
                          ? (shown / duration) : 0.0;

    const int grooveW = groove.right - groove.left;
    int filled = (int)(grooveW * (fraction < 0.0 ? 0.0 : (fraction > 1.0 ? 1.0 : fraction)));

    if (filled > 0)
    {
        RECT played = groove;
        played.right = groove.left + filled;
        FillRounded(dc, played, radius, kColPlayed);
    }

    const int knobX = groove.left + filled;
    const int knobY = (groove.top + groove.bottom) / 2;
    const int knobR = S(kKnobR);
    {
        HBRUSH brush = CreateSolidBrush(kColKnob);
        HGDIOBJ oldBrush = SelectObject(dc, brush);
        HGDIOBJ oldPen   = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, knobX - knobR, knobY - knobR, knobX + knobR + 1, knobY + knobR + 1);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(brush);
    }

    // --- play/pause glyph, which is also the button ------------------------
    const RECT glyph = GlyphRect();
    {
        HBRUSH brush = CreateSolidBrush(kColText);
        HGDIOBJ oldBrush = SelectObject(dc, brush);
        HGDIOBJ oldPen   = SelectObject(dc, GetStockObject(NULL_PEN));

        if (IsPlaying())
        {
            // Playing: the button pauses, so it shows two bars.
            const int barW = (glyph.right - glyph.left) * 2 / 5;
            RECT a = { glyph.left, glyph.top, glyph.left + barW, glyph.bottom };
            RECT b = { glyph.right - barW, glyph.top, glyph.right, glyph.bottom };
            Rectangle(dc, a.left, a.top, a.right + 1, a.bottom + 1);
            Rectangle(dc, b.left, b.top, b.right + 1, b.bottom + 1);
        }
        else
        {
            POINT tri[3];
            tri[0].x = glyph.left;  tri[0].y = glyph.top;
            tri[1].x = glyph.right; tri[1].y = (glyph.top + glyph.bottom) / 2;
            tri[2].x = glyph.left;  tri[2].y = glyph.bottom;
            Polygon(dc, tri, 3);
        }

        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(brush);
    }

    // --- readout and hints -------------------------------------------------
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);

    wchar_t line[160];
    swprintf_s(line, L"%s / %s      frame %lld",
               FormatTime(shown).c_str(),
               FormatTime(duration).c_str(),
               FrameAt(shown));

    RECT text = bar;
    text.left  = glyph.right + S(12);
    text.top   = bar.top + S(kTextY) - S(2);
    text.right = bar.right - S(kBarPadX);

    SetTextColor(dc, kColText);
    DrawTextW(dc, line, -1, &text, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);

    const wchar_t* hint =
        L"Space  play/pause     Left/Right  step a frame     F  fullscreen     Esc  close";

    // Drop the key hints rather than let them run into the readout on a narrow
    // window; the readout is the part that has to stay legible.
    SIZE readout = { 0, 0 };
    SIZE keys    = { 0, 0 };
    GetTextExtentPoint32W(dc, line, (int)wcslen(line), &readout);
    GetTextExtentPoint32W(dc, hint, (int)wcslen(hint), &keys);

    if (readout.cx + keys.cx + S(24) <= text.right - text.left)
    {
        SetTextColor(dc, kColHint);
        DrawTextW(dc, hint, -1, &text, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
    }

    SelectObject(dc, oldFont);
}

void PlayerWindow::Impl::TogglePlay()
{
    if (!engine)
        return;

    if (engine->IsPaused())
    {
        // Replaying after the end has to rewind first, or Play is a no-op.
        if (ended || engine->IsEnded())
        {
            engine->SetCurrentTime(0.0);
            ended = false;
        }
        engine->Play();
    }
    else
    {
        engine->Pause();
    }

    InvalidateRect(hwnd, NULL, FALSE);
}

void PlayerWindow::Impl::StepFrame(int direction)
{
    if (!engine)
        return;

    // Both directions go through a seek rather than IMFMediaEngineEx::FrameStep,
    // which only moves forward. One mechanism means one step size and no surprise
    // when the direction changes.
    if (!engine->IsPaused())
        engine->Pause();

    const double step = (opts.fpsNum > 0 && opts.fpsDen > 0)
                      ? (double)opts.fpsDen / (double)opts.fpsNum
                      : (1.0 / 60.0);

    double t = engine->GetCurrentTime();
    if (!Finite(t))
        t = 0.0;

    t += step * direction;
    if (t < 0.0) t = 0.0;
    if (duration > 0.0 && t > duration) t = duration;

    ended = false;
    engine->SetCurrentTime(t);
    position = t;
    InvalidateRect(hwnd, NULL, FALSE);
}

void PlayerWindow::Impl::SeekToX(int x, bool commit)
{
    if (!engine || duration <= 0.0)
        return;

    const RECT groove = GrooveRect();
    const int width = groove.right - groove.left;
    if (width <= 0)
        return;

    double f = (double)(x - groove.left) / (double)width;
    if (f < 0.0) f = 0.0;
    if (f > 1.0) f = 1.0;

    position = f * duration;
    ended = false;

    // Dropping seeks while one is still in flight keeps a drag responsive instead
    // of queueing up a seek per mouse message. The release always commits.
    if (commit || !engine->IsSeeking())
        engine->SetCurrentTime(position);

    InvalidateRect(hwnd, NULL, FALSE);
}

void PlayerWindow::Impl::ToggleFullscreen()
{
    if (!fullscreen)
    {
        placement.length = sizeof(placement);
        if (!GetWindowPlacement(hwnd, &placement))
            return;

        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(mon, &mi))
            return;

        styleBeforeFullscreen = GetWindowLongW(hwnd, GWL_STYLE);
        SetWindowLongW(hwnd, GWL_STYLE,
                       (styleBeforeFullscreen & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);

        SetWindowPos(hwnd, HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED);
        fullscreen = true;
        lastMouseTick = GetTickCount();
    }
    else
    {
        SetWindowLongW(hwnd, GWL_STYLE, styleBeforeFullscreen);
        SetWindowPlacement(hwnd, &placement);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        fullscreen = false;
        ShowBar(true);
    }

    Layout();
}

// The transport is a reserved strip rather than an overlay: the engine composites
// its own surface onto the video window, so GDI drawn on top of it would be hidden.
// Hiding the strip therefore reflows the video instead of uncovering it.
void PlayerWindow::Impl::ShowBar(bool show)
{
    if (barVisible == show)
        return;
    barVisible = show;
    Layout();
}

void PlayerWindow::Impl::FailWith(const std::wstring& message)
{
    if (error.empty())
        error = message;
    if (hwnd)
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

void PlayerWindow::Impl::OnEngineEvent(DWORD event, HRESULT param2)
{
    switch (event)
    {
    case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:
        {
            DWORD w = 0, h = 0;
            if (engine && SUCCEEDED(engine->GetNativeVideoSize(&w, &h)) && w && h)
            {
                videoW = (int)w;
                videoH = (int)h;
                SizeToVideo();
            }
            const double d = engine ? engine->GetDuration() : 0.0;
            if (Finite(d) && d > 0.0)
                duration = d;
            Layout();
        }
        break;

    case MF_MEDIA_ENGINE_EVENT_DURATIONCHANGE:
        {
            const double d = engine ? engine->GetDuration() : 0.0;
            if (Finite(d) && d > 0.0)
                duration = d;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;

    case MF_MEDIA_ENGINE_EVENT_FIRSTFRAMEREADY:
        Layout();
        break;

    case MF_MEDIA_ENGINE_EVENT_PLAY:
    case MF_MEDIA_ENGINE_EVENT_PLAYING:
        ended = false;
        InvalidateRect(hwnd, NULL, FALSE);
        break;

    case MF_MEDIA_ENGINE_EVENT_PAUSE:
    case MF_MEDIA_ENGINE_EVENT_SEEKED:
        InvalidateRect(hwnd, NULL, FALSE);
        break;

    case MF_MEDIA_ENGINE_EVENT_ENDED:
        ended = true;
        ShowBar(true);
        InvalidateRect(hwnd, NULL, FALSE);
        break;

    case MF_MEDIA_ENGINE_EVENT_ERROR:
        {
            MF_MEDIA_ENGINE_ERR code = MF_MEDIA_ENGINE_ERR_NOERROR;
            HRESULT ext = param2;

            IMFMediaError* err = NULL;
            if (engine && SUCCEEDED(engine->GetError(&err)) && err)
            {
                code = (MF_MEDIA_ENGINE_ERR)err->GetErrorCode();
                const HRESULT reported = err->GetExtendedErrorCode();
                if (reported != 0)
                    ext = reported;
                err->Release();
            }

            // Roomy enough for the longest message with a maximum-length file name;
            // swprintf_s terminates the process rather than truncating.
            wchar_t buf[1024];
            const wchar_t* name = PathFindFileNameW(path.c_str());

            if (IsMissingDecoder(code, ext))
            {
                swprintf_s(buf,
                    L"Windows could not decode the video in %s. MilkRun writes H.265 "
                    L"(HEVC), which needs the HEVC Video Extensions from the Microsoft "
                    L"Store. Install those and the file will play. The render itself "
                    L"is finished and on disk either way.", name);
            }
            else if (code == MF_MEDIA_ENGINE_ERR_NETWORK)
            {
                swprintf_s(buf, L"Could not read %s from disk (0x%08X).",
                           name, (unsigned)ext);
            }
            else
            {
                swprintf_s(buf, L"Could not play %s (0x%08X).", name, (unsigned)ext);
            }

            FailWith(buf);
        }
        break;

    default:
        break;
    }
}

void PlayerWindow::Impl::ShutdownEngine()
{
    // Order matters. Detach first so no worker thread can post to a window that is
    // about to go away, then shut the engine down while its window still exists,
    // then drop the references.
    if (notify)
        notify->Detach();

    if (engineEx)
    {
        engineEx->Release();
        engineEx = NULL;
    }

    if (engine)
    {
        engine->Shutdown();
        engine->Release();
        engine = NULL;
    }

    if (notify)
    {
        notify->Release();
        notify = NULL;
    }
}

LRESULT CALLBACK PlayerWindow::Impl::VideoProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    Impl* d = (Impl*)GetWindowLongPtrW(h, GWLP_USERDATA);

    switch (msg)
    {
    case WM_CREATE:
        SetWindowLongPtrW(h, GWLP_USERDATA,
                          (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
        return 0;

    case WM_LBUTTONDBLCLK:
        if (d)
            d->ToggleFullscreen();
        return 0;

    case WM_MOUSEMOVE:
        // The video covers most of the window, so idle detection has to see its
        // mouse traffic too.
        if (d)
        {
            d->lastMouseTick = GetTickCount();
            if (d->fullscreen)
                d->ShowBar(true);
        }
        return 0;

    case WM_SETCURSOR:
        if (d && d->fullscreen && !d->barVisible)
        {
            SetCursor(NULL);
            return TRUE;
        }
        break;

    case WM_ERASEBKGND:
        // The engine owns every pixel here once the first frame lands.
        return 1;
    }

    return DefWindowProcW(h, msg, w, l);
}

LRESULT CALLBACK PlayerWindow::Impl::PlayerProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    Impl* d = (Impl*)GetWindowLongPtrW(h, GWLP_USERDATA);

    switch (msg)
    {
    case WM_CREATE:
        SetWindowLongPtrW(h, GWLP_USERDATA,
                          (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
        return 0;

    case WM_SIZE:
        if (d)
            d->Layout();
        return 0;

    case WM_GETMINMAXINFO:
        // Arrives before WM_CREATE has handed over the state, so only clamp once
        // there is a dpi to scale by.
        if (d)
        {
            MINMAXINFO* mmi = (MINMAXINFO*)l;
            mmi->ptMinTrackSize.x = d->S(420);
            mmi->ptMinTrackSize.y = d->S(kBarHeight + 120);
            return 0;
        }
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        if (d)
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);

            RECT client;
            GetClientRect(h, &client);

            if (client.right > 0 && client.bottom > 0)
            {
                // Double buffered, because the transport redraws several times a
                // second and the letterbox behind it must not flash.
                HDC     mem = CreateCompatibleDC(dc);
                HBITMAP bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
                HGDIOBJ old = SelectObject(mem, bmp);

                d->Paint(mem, client);
                BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);

                SelectObject(mem, old);
                DeleteObject(bmp);
                DeleteDC(mem);
            }

            EndPaint(h, &ps);
        }
        return 0;

    case WM_TIMER:
        if (d && w == kTimerTick)
        {
            if (d->barVisible)
            {
                const RECT bar = d->BarRect();
                InvalidateRect(h, &bar, FALSE);
            }
            if (d->fullscreen && d->barVisible && !d->scrubbing &&
                d->IsPlaying() &&
                (GetTickCount() - d->lastMouseTick) > kIdleHideMs)
            {
                d->ShowBar(false);
            }
        }
        return 0;

    case WM_MOUSEMOVE:
        if (d)
        {
            d->lastMouseTick = GetTickCount();
            if (d->fullscreen)
                d->ShowBar(true);
            if (d->scrubbing)
                d->SeekToX(GET_X_LPARAM(l), false);
        }
        return 0;

    case WM_SETCURSOR:
        if (d && d->fullscreen && !d->barVisible)
        {
            SetCursor(NULL);
            return TRUE;
        }
        break;

    case WM_LBUTTONDOWN:
        if (d)
        {
            POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            const RECT glyph = d->GlyphRect();
            const RECT seek  = d->SeekHitRect();

            if (d->barVisible && PtInRect(&glyph, pt))
            {
                d->TogglePlay();
            }
            else if (d->barVisible && PtInRect(&seek, pt))
            {
                d->scrubbing = true;
                SetCapture(h);
                d->SeekToX(pt.x, false);
            }
        }
        return 0;

    case WM_LBUTTONUP:
        if (d && d->scrubbing)
        {
            d->scrubbing = false;
            ReleaseCapture();
            d->SeekToX(GET_X_LPARAM(l), true);
        }
        return 0;

    case WM_CAPTURECHANGED:
        if (d)
            d->scrubbing = false;
        return 0;

    case WM_LBUTTONDBLCLK:
        if (d)
        {
            POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            const RECT bar = d->BarRect();
            if (!d->barVisible || !PtInRect(&bar, pt))
                d->ToggleFullscreen();
        }
        return 0;

    case WM_KEYDOWN:
        if (d)
        {
            switch (w)
            {
            case VK_SPACE:  d->TogglePlay();       return 0;
            case VK_LEFT:   d->StepFrame(-1);      return 0;
            case VK_RIGHT:  d->StepFrame(+1);      return 0;
            case VK_HOME:
                if (d->engine)
                {
                    d->ended = false;
                    d->engine->SetCurrentTime(0.0);
                    d->position = 0.0;
                }
                return 0;
            case 'F':       d->ToggleFullscreen(); return 0;
            case VK_ESCAPE: PostMessageW(h, WM_CLOSE, 0, 0); return 0;
            default: break;
            }
        }
        break;

    case WM_PLAYER_ENGINE_EVENT:
        if (d)
            d->OnEngineEvent((DWORD)w, (HRESULT)l);
        return 0;

    case WM_CLOSE:
        if (d)
        {
            // The engine has to be shut down before its video window is destroyed.
            KillTimer(h, kTimerTick);
            d->ShutdownEngine();
        }
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        if (d)
            d->quit = true;
        return 0;
    }

    return DefWindowProcW(h, msg, w, l);
}

PlayerWindow::PlayerWindow() : m_impl(new Impl()) {}

PlayerWindow::~PlayerWindow()
{
    Impl& d = *m_impl;

    d.ShutdownEngine();

    if (d.hwnd)
    {
        DestroyWindow(d.hwnd);
        d.hwnd = NULL;
    }
    if (d.font)
    {
        DeleteObject(d.font);
        d.font = NULL;
    }
    if (d.mfStarted)
    {
        MFShutdown();
        d.mfStarted = false;
    }
    if (d.comInited)
    {
        CoUninitialize();
        d.comInited = false;
    }

    delete m_impl;
    m_impl = NULL;
}

bool PlayerWindow::Show(HINSTANCE hInstance, const std::wstring& path,
                        const PlayerOptions& opts, std::wstring& error)
{
    Impl& d = *m_impl;

    if (d.hwnd)
    {
        error = L"This player is already open.";
        return false;
    }

    d.hinst = hInstance;
    d.opts  = opts;
    d.path  = path;
    d.quit  = false;
    d.error.clear();

    if (path.empty())
    {
        error = L"No file to play.";
        return false;
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        error = L"There is no file at " + path + L".";
        return false;
    }

    // S_FALSE still adds a reference, so it has to be balanced. RPC_E_CHANGED_MODE
    // means the thread is already an apartment of the other kind, which the media
    // engine is happy with; leave that initializer alone.
    if (!d.comInited)
        d.comInited = SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED));

    if (!d.mfStarted)
    {
        if (FAILED(MFStartup(MF_VERSION)))
        {
            error = L"Could not start Media Foundation, so the built-in player cannot run.";
            return false;
        }
        d.mfStarted = true;
    }

    static bool classesRegistered = false;
    if (!classesRegistered)
    {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_DBLCLKS;
        wc.lpfnWndProc   = Impl::PlayerProc;
        wc.hInstance     = hInstance;
        // IDC_ARROW resolves to the ANSI ordinal in this build, and a cursor handle
        // is a cursor handle either way.
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.lpszClassName = kPlayerClass;
        wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_PLUGIN_ICON));
        RegisterClassExW(&wc);

        wc.lpfnWndProc   = Impl::VideoProc;
        wc.lpszClassName = kVideoClass;
        wc.hIcon         = NULL;
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RegisterClassExW(&wc);

        classesRegistered = true;
    }

    std::wstring caption = opts.title;
    if (caption.empty())
        caption = PathFindFileNameW(path.c_str());

    d.hwnd = CreateWindowExW(0, kPlayerClass, caption.c_str(),
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 960, 594,
                             NULL, NULL, hInstance, &d);
    if (!d.hwnd)
    {
        error = L"Could not create the player window.";
        return false;
    }

    {
        HDC dc = GetDC(d.hwnd);
        if (dc)
        {
            const int measured = GetDeviceCaps(dc, LOGPIXELSX);
            if (measured > 0)
                d.dpi = measured;
            ReleaseDC(d.hwnd, dc);
        }
    }

    if (d.font)
        DeleteObject(d.font);

    d.font = CreateFontW(-MulDiv(9, d.dpi, 72), 0, 0, 0, FW_NORMAL,
                         FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                         L"Segoe UI");

    d.hwndVideo = CreateWindowExW(0, kVideoClass, L"",
                                  WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                  0, 0, 16, 9,
                                  d.hwnd, NULL, hInstance, &d);
    if (!d.hwndVideo)
    {
        error = L"Could not create the video surface.";
        return false;
    }

    IMFMediaEngineClassFactory* factory = NULL;
    HRESULT hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, NULL,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        error = L"Windows Media Foundation's media engine is not available on this "
                L"system, so the built-in player cannot run. On an N edition of "
                L"Windows this needs the Media Feature Pack.";
        return false;
    }

    d.notify = new EngineNotify(d.hwnd);

    IMFAttributes* attrs = NULL;
    hr = MFCreateAttributes(&attrs, 2);
    if (SUCCEEDED(hr))
    {
        // SetUnknown takes its own reference, so the engine keeps the callback
        // alive independently of the reference this object holds.
        attrs->SetUnknown(MF_MEDIA_ENGINE_CALLBACK,
                          static_cast<IMFMediaEngineNotify*>(d.notify));
        attrs->SetUINT64(MF_MEDIA_ENGINE_PLAYBACK_HWND,
                         (UINT64)(ULONG_PTR)d.hwndVideo);

        hr = factory->CreateInstance(0, attrs, &d.engine);
        attrs->Release();
    }
    factory->Release();

    if (FAILED(hr) || !d.engine)
    {
        error = L"Could not create the media engine for playback.";
        return false;
    }

    d.engine->QueryInterface(IID_PPV_ARGS(&d.engineEx));   // optional

    d.engine->SetAutoPlay(opts.startPlaying ? TRUE : FALSE);

    BSTR source = SysAllocString(path.c_str());
    if (!source)
    {
        error = L"Out of memory opening the file for playback.";
        return false;
    }
    hr = d.engine->SetSource(source);
    SysFreeString(source);

    if (FAILED(hr))
    {
        wchar_t buf[320];
        swprintf_s(buf, L"Could not open %s for playback (0x%08X).",
                   PathFindFileNameW(path.c_str()), (unsigned)hr);
        error = buf;
        return false;
    }

    ShowWindow(d.hwnd, SW_SHOW);
    SetForegroundWindow(d.hwnd);
    SetFocus(d.hwnd);
    d.Layout();

    if (opts.startFullscreen)
        d.ToggleFullscreen();

    SetTimer(d.hwnd, kTimerTick, kTickMs, NULL);
    d.lastMouseTick = GetTickCount();

    // Deliberately not PostQuitMessage on destroy: this player can be opened from
    // inside an app that has its own message loop to go back to, and a stray WM_QUIT
    // would tear that down too.
    MSG msg;
    while (!d.quit)
    {
        if (!GetMessageW(&msg, NULL, 0, 0))
            break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    d.hwnd      = NULL;   // destroyed by the loop
    d.hwndVideo = NULL;

    if (!d.error.empty())
    {
        error = d.error;
        return false;
    }
    return true;
}

bool PlayFile(HINSTANCE hInstance, const std::wstring& path,
              int fpsNum, int fpsDen, std::wstring& error)
{
    PlayerOptions opts;
    if (fpsNum > 0 && fpsDen > 0)
    {
        opts.fpsNum = fpsNum;
        opts.fpsDen = fpsDen;
    }

    PlayerWindow player;
    return player.Show(hInstance, path, opts, error);
}

} // namespace offline
