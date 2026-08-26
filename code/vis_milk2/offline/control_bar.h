#ifndef __MILKRUN_CONTROL_BAR_H__
#define __MILKRUN_CONTROL_BAR_H__ 1

#include <windows.h>

namespace app {

// A strip of labelled buttons across the top of the visualizer window.
//
// It is an owned popup rather than a child window, because the visualizer
// Presents to the whole client area and would paint straight over a child on
// every frame. An owned popup is composited above the owner instead, so it
// survives without the render path having to reserve space or crop.
//
// It hides itself while the mouse is idle and comes back on movement, so it does
// not sit permanently over the visualization.
class ControlBar
{
public:
    // Sent to the owner as WM_COMMAND with these in the low word of wParam.
    enum Command
    {
        CmdRender = 0x9100,
        CmdSettings,
        CmdPresets,
        CmdPlay,
        CmdHelp
    };

    ControlBar();
    ~ControlBar();

    bool Create(HINSTANCE hInstance, HWND owner);
    void Destroy();

    // Follows the owner. Call on move, size and any style change.
    void Reposition();

    // Fullscreen hides the bar until the pointer comes near the top edge.
    void SetFullscreen(bool fullscreen);

    // Owner forwards mouse movement so the bar can wake up.
    void NotifyMouseActivity();

    HWND Hwnd() const { return m_hwnd; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(HWND, UINT, WPARAM, LPARAM);

    void Paint(HDC dc);
    int  HitTest(POINT pt) const;
    void Layout();

    struct Button
    {
        Command      cmd;
        const wchar_t* label;
        RECT         rect;
    };

    HWND      m_hwnd;
    HWND      m_owner;
    HFONT     m_font;
    Button*   m_buttons;
    int       m_count;
    int       m_hot;        // index under the pointer, or -1
    int       m_pressed;
    bool      m_fullscreen;
    DWORD     m_lastActivity;
    bool      m_hidden;
    int       m_dpi;
};

} // namespace app

#endif
