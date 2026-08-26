#ifndef __MILKRUN_PLAYER_WINDOW_H__
#define __MILKRUN_PLAYER_WINDOW_H__ 1

#include <windows.h>
#include <string>

namespace offline {

struct PlayerOptions
{
    // Window caption. Empty means use the file name.
    std::wstring title;

    // How far one frame step moves. Nothing in an mp4 exposes the frame rate
    // cheaply and exactly, and the caller just finished encoding at a known rate,
    // so it passes that in rather than having the player guess.
    int fpsNum = 60;
    int fpsDen = 1;

    bool startPlaying    = true;
    bool startFullscreen = false;
};

// A review player for a finished render: Media Foundation's media engine driving
// its own window, with a GDI transport bar.
//
// Show blocks. It creates the window, runs its own message loop and returns when
// the user closes it, which is fine because it only ever runs after a render, never
// beside one.
class PlayerWindow
{
public:
    PlayerWindow();
    ~PlayerWindow();

    // Returns false and fills 'error' when the file could not be played, including
    // the asynchronous case where the engine reports the failure after loading has
    // already started. Closing the window normally returns true.
    bool Show(HINSTANCE hInstance, const std::wstring& path,
              const PlayerOptions& opts, std::wstring& error);

private:
    PlayerWindow(const PlayerWindow&);
    PlayerWindow& operator=(const PlayerWindow&);

    struct Impl;
    Impl* m_impl;
};

// The whole thing in one call, for "the render finished, show it".
bool PlayFile(HINSTANCE hInstance, const std::wstring& path,
              int fpsNum, int fpsDen, std::wstring& error);

} // namespace offline

#endif
