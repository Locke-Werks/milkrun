#include "control_bar.h"
#include <windowsx.h>

namespace app {

namespace {

const wchar_t kClassName[] = L"MilkRunControlBar";

// Dark, so it reads against a visualization that is usually dark, and flat so it
// does not compete with what is behind it.
const COLORREF kBack      = RGB(18, 18, 21);
const COLORREF kText      = RGB(228, 228, 232);
const COLORREF kTextHot   = RGB(255, 255, 255);
const COLORREF kHot       = RGB(44, 44, 52);
const COLORREF kPressed   = RGB(214, 38, 42);
const COLORREF kEdge      = RGB(44, 44, 50);

const int kIdleTimer   = 1;
const int kTrackTimer  = 2;

// How long the pointer has to sit still before the bar gets out of the way.
const DWORD kIdleHideMs = 2500;

int Scale(int v, int dpi) { return MulDiv(v, dpi, 96); }

} // namespace

ControlBar::ControlBar()
    : m_hwnd(NULL), m_owner(NULL), m_font(NULL), m_buttons(NULL), m_count(0),
      m_hot(-1), m_pressed(-1), m_fullscreen(false), m_lastActivity(0),
      m_hidden(false), m_dpi(96)
{
}

ControlBar::~ControlBar()
{
    Destroy();
}

bool ControlBar::Create(HINSTANCE hInstance, HWND owner)
{
    Destroy();
    m_owner = owner;

    static Button buttons[] = {
        { CmdRender,   L"Render Video",  { 0, 0, 0, 0 } },
        { CmdPresets,  L"Presets",       { 0, 0, 0, 0 } },
        { CmdSettings, L"Settings",      { 0, 0, 0, 0 } },
        { CmdPlay,     L"Play Video",    { 0, 0, 0, 0 } },
        { CmdHelp,     L"Help",          { 0, 0, 0, 0 } },
    };
    m_buttons = buttons;
    m_count = ARRAYSIZE(buttons);

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc   = ControlBar::WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = NULL;   // painted whole in WM_PAINT, so no flicker
    wc.lpszClassName = kClassName;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    // WS_EX_NOACTIVATE keeps clicks from pulling focus off the visualizer, which
    // would otherwise interrupt its keyboard handling every time a button is hit.
    m_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kClassName, L"", WS_POPUP,
        0, 0, 10, 10,
        owner, NULL, hInstance, this);

    if (!m_hwnd)
        return false;

    m_dpi = 96;
    HDC dc = GetDC(m_hwnd);
    if (dc) { m_dpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(m_hwnd, dc); }

    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight  = -MulDiv(10, m_dpi, 72);
    lf.lfWeight  = FW_NORMAL;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpynW(lf.lfFaceName, L"Segoe UI", LF_FACESIZE);
    m_font = CreateFontIndirectW(&lf);

    Layout();
    Reposition();
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);

    m_lastActivity = GetTickCount();
    SetTimer(m_hwnd, kIdleTimer, 500, NULL);
    return true;
}

void ControlBar::Destroy()
{
    if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = NULL; }
    if (m_font) { DeleteObject(m_font); m_font = NULL; }
    m_owner = NULL;
}

void ControlBar::Layout()
{
    if (!m_hwnd || !m_buttons)
        return;

    HDC dc = GetDC(m_hwnd);
    HGDIOBJ old = SelectObject(dc, m_font);

    const int padX = Scale(16, m_dpi);
    const int gap  = Scale(2, m_dpi);
    int x = Scale(6, m_dpi);
    const int h = Scale(30, m_dpi);

    for (int i = 0; i < m_count; i++)
    {
        SIZE s;
        GetTextExtentPoint32W(dc, m_buttons[i].label, lstrlenW(m_buttons[i].label), &s);
        const int w = s.cx + padX * 2;
        SetRect(&m_buttons[i].rect, x, Scale(3, m_dpi), x + w, Scale(3, m_dpi) + h);
        x += w + gap;
    }

    SelectObject(dc, old);
    ReleaseDC(m_hwnd, dc);
}

void ControlBar::Reposition()
{
    if (!m_hwnd || !m_owner)
        return;

    RECT c;
    GetClientRect(m_owner, &c);
    POINT tl; tl.x = c.left; tl.y = c.top;
    ClientToScreen(m_owner, &tl);

    const int barH = Scale(36, m_dpi);
    const int barW = (c.right - c.left);

    SetWindowPos(m_hwnd, HWND_TOP, tl.x, tl.y, barW, barH,
                 SWP_NOACTIVATE | (m_hidden ? SWP_HIDEWINDOW : SWP_SHOWWINDOW));
}

void ControlBar::SetFullscreen(bool fullscreen)
{
    m_fullscreen = fullscreen;
    NotifyMouseActivity();
    Reposition();
}

void ControlBar::NotifyMouseActivity()
{
    m_lastActivity = GetTickCount();
    if (m_hidden)
    {
        m_hidden = false;
        if (m_hwnd) ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    }
}

int ControlBar::HitTest(POINT pt) const
{
    for (int i = 0; i < m_count; i++)
        if (PtInRect(&m_buttons[i].rect, pt))
            return i;
    return -1;
}

void ControlBar::Paint(HDC dc)
{
    RECT c;
    GetClientRect(m_hwnd, &c);

    HBRUSH back = CreateSolidBrush(kBack);
    FillRect(dc, &c, back);
    DeleteObject(back);

    // A hairline under the bar separates it from the visualization.
    RECT edge = c;
    edge.top = edge.bottom - 1;
    HBRUSH e = CreateSolidBrush(kEdge);
    FillRect(dc, &edge, e);
    DeleteObject(e);

    HGDIOBJ oldFont = SelectObject(dc, m_font);
    SetBkMode(dc, TRANSPARENT);

    for (int i = 0; i < m_count; i++)
    {
        RECT r = m_buttons[i].rect;

        if (i == m_pressed || i == m_hot)
        {
            HBRUSH b = CreateSolidBrush(i == m_pressed ? kPressed : kHot);
            FillRect(dc, &r, b);
            DeleteObject(b);
        }

        SetTextColor(dc, (i == m_hot || i == m_pressed) ? kTextHot : kText);
        DrawTextW(dc, m_buttons[i].label, -1, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    SelectObject(dc, oldFont);
}

LRESULT CALLBACK ControlBar::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    ControlBar* self = (ControlBar*)GetWindowLongPtrW(h, GWLP_USERDATA);

    if (msg == WM_NCCREATE)
    {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        self = (ControlBar*)cs->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        if (self) self->m_hwnd = h;
    }

    if (self)
        return self->Handle(h, msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT ControlBar::Handle(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);

        // Double buffered: the bar repaints on every hover change and would
        // otherwise flicker over a moving background.
        RECT c;
        GetClientRect(h, &c);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, c.right, c.bottom);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        Paint(mem);
        BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);

        EndPaint(h, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;   // WM_PAINT covers the whole surface

    case WM_MOUSEMOVE:
    {
        POINT pt; pt.x = GET_X_LPARAM(lp); pt.y = GET_Y_LPARAM(lp);
        const int hot = HitTest(pt);
        NotifyMouseActivity();
        if (hot != m_hot)
        {
            m_hot = hot;
            InvalidateRect(h, NULL, FALSE);

            TRACKMOUSEEVENT tme;
            ZeroMemory(&tme, sizeof(tme));
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = h;
            TrackMouseEvent(&tme);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        m_hot = -1;
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_LBUTTONDOWN:
    {
        POINT pt; pt.x = GET_X_LPARAM(lp); pt.y = GET_Y_LPARAM(lp);
        m_pressed = HitTest(pt);
        if (m_pressed >= 0) { SetCapture(h); InvalidateRect(h, NULL, FALSE); }
        return 0;
    }

    case WM_LBUTTONUP:
    {
        POINT pt; pt.x = GET_X_LPARAM(lp); pt.y = GET_Y_LPARAM(lp);
        const int was = m_pressed;
        m_pressed = -1;
        if (GetCapture() == h) ReleaseCapture();
        InvalidateRect(h, NULL, FALSE);

        if (was >= 0 && HitTest(pt) == was && m_owner)
            PostMessageW(m_owner, WM_COMMAND, MAKEWPARAM(m_buttons[was].cmd, 0), 0);
        return 0;
    }

    case WM_TIMER:
        if (wp == kIdleTimer)
        {
            // Windowed, the bar stays put: it is the only thing telling a new
            // user that rendering exists, and hiding it on a timer is how the
            // feature went unfound in the first place. Fullscreen is different,
            // because there the whole point is an unobstructed picture.
            if (m_fullscreen && !m_hidden && m_hot < 0 && m_pressed < 0 &&
                (GetTickCount() - m_lastActivity) > kIdleHideMs)
            {
                m_hidden = true;
                ShowWindow(h, SW_HIDE);
            }
        }
        return 0;

    case WM_DESTROY:
        KillTimer(h, kIdleTimer);
        return 0;
    }

    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace app
