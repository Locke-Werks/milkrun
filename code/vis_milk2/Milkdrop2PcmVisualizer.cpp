#ifdef DEBUG
#define _CRTDBG_MAP_ALLOC
#endif

#include <stdlib.h>
#include <malloc.h>
#include <crtdbg.h>

#include <windows.h>
#include <process.h>
#include <d3d9.h>
#include <math.h>
#include <dwmapi.h>

// SPOUT
#include <ShellScalingApi.h> // for dpi awareness
#pragma comment(lib, "shcore.lib") // for dpi awareness

#include "plugin.h"
#include "app_device.h"
#include "offline/cli.h"
#include "offline/render_dialog.h"
#include "offline/settings_dialog.h"
#include "offline/first_run.h"
#include "offline/control_bar.h"
#include "offline/player_window.h"
#include <commdlg.h>
#include "resource.h"

#include <mutex>
#include <atomic>

//#include <core/sdk/constants.h>
//#include <core/sdk/IPcmVisualizer.h>
//#include <core/sdk/IPlaybackRemote.h>

#include "..\audio\common.h"

#define DLL_EXPORT __declspec(dllexport)
//#define COMPILE_AS_DLL
#define SAMPLE_SIZE 576
#define DEFAULT_WIDTH 800;
#define DEFAULT_HEIGHT 800;

CPlugin g_plugin;
HINSTANCE api_orig_hinstance = nullptr;
_locale_t g_use_C_locale;
char keyMappings[8];

static IDirect3D9* pD3D9 = nullptr;
IDirect3DDevice9* pD3DDevice = nullptr;
D3DPRESENT_PARAMETERS d3dPp;

static LONG lastWindowStyle = 0;
static LONG lastWindowStyleEx = 0;

static bool fullscreen = false;
static bool stretch = false;
static RECT lastRect = { 0 };

static HMODULE module = nullptr;
static std::atomic<HANDLE> thread = nullptr;
static unsigned threadId = 0;
static std::mutex pcmMutex;
static unsigned char pcmLeftIn[SAMPLE_SIZE];
static unsigned char pcmRightIn[SAMPLE_SIZE];
static unsigned char pcmLeftOut[SAMPLE_SIZE];
static unsigned char pcmRightOut[SAMPLE_SIZE];

//static musik::core::sdk::IPlaybackService* playback = nullptr;

static HICON icon = nullptr;
static app::ControlBar g_controlBar;

// Pick the adapter to render on. Machines commonly expose a virtual display
// adapter (remote desktop, capture drivers) alongside the real GPU, and it sorts
// first as often as not, so prefer a real vendor. NVIDIA leads because it is the
// only one that can also serve NVENC.
static UINT PickPreferredAdapter(IDirect3D9* pD3D) {
    const UINT count = pD3D->GetAdapterCount();
    const DWORD kVendorNvidia = 0x10DE, kVendorAmd = 0x1002, kVendorIntel = 0x8086;
    const DWORD order[] = { kVendorNvidia, kVendorAmd, kVendorIntel };

    for (int pass = 0; pass < ARRAYSIZE(order); pass++) {
        for (UINT i = 0; i < count; i++) {
            D3DADAPTER_IDENTIFIER9 id = { 0 };
            if (SUCCEEDED(pD3D->GetAdapterIdentifier(i, 0, &id)) && id.VendorId == order[pass])
                return i;
        }
    }
    return D3DADAPTER_DEFAULT;
}

// bOffline adds the two device flags NVENC requires on top of the one the app has
// always used, and drops vsync so an offline render is not pinned to refresh rate.
// D3DCREATE_FPU_PRESERVE is wanted regardless: ns-eel2 evaluates preset equations
// on the x87 FPU and D3D otherwise switches it to single precision.
bool InitD3d(HWND hwnd, int width, int height, bool bOffline, D3DFORMAT preferredBackBufFormat) {
    pD3D9 = Direct3DCreate9(D3D_SDK_VERSION);
    if (!pD3D9)
        return false;

    UINT adapterId = g_plugin.m_adapterId;
    if (adapterId == 0 || adapterId >= pD3D9->GetAdapterCount())
        adapterId = PickPreferredAdapter(pD3D9);

    D3DDISPLAYMODE mode;
    if (FAILED(pD3D9->GetAdapterDisplayMode(adapterId, &mode))) {
        DeinitD3d();
        return false;
    }

    // Honour a wider back buffer only if the adapter can actually present it.
    D3DFORMAT backBufFormat = mode.Format;
    if (preferredBackBufFormat != D3DFMT_UNKNOWN &&
        SUCCEEDED(pD3D9->CheckDeviceType(adapterId, D3DDEVTYPE_HAL, mode.Format, preferredBackBufFormat, TRUE)))
    {
        backBufFormat = preferredBackBufFormat;
    }

    memset(&d3dPp, 0, sizeof(d3dPp));

    d3dPp.BackBufferCount = 1;
    d3dPp.BackBufferFormat = backBufFormat;
    d3dPp.BackBufferWidth = width;
    d3dPp.BackBufferHeight = height;
    d3dPp.SwapEffect = D3DSWAPEFFECT_COPY;
    d3dPp.Flags = 0;
    d3dPp.EnableAutoDepthStencil = TRUE;
    d3dPp.AutoDepthStencilFormat = D3DFMT_D24X8;
    d3dPp.Windowed = TRUE;
    d3dPp.PresentationInterval = bOffline ? D3DPRESENT_INTERVAL_IMMEDIATE : D3DPRESENT_INTERVAL_ONE;
    d3dPp.MultiSampleType = D3DMULTISAMPLE_NONE;
    d3dPp.hDeviceWindow = (HWND) hwnd;

    DWORD flags = D3DCREATE_HARDWARE_VERTEXPROCESSING;
    if (bOffline)
        flags |= D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED;

    HRESULT hr = pD3D9->CreateDevice(
        adapterId,
        D3DDEVTYPE_HAL,
        (HWND) hwnd,
        flags,
        &d3dPp,
        &pD3DDevice);

    if (FAILED(hr)) {
        DeinitD3d();
        return false;
    }
    return true;
}

void DeinitD3d() {
    if (pD3DDevice) {
        pD3DDevice->Release();
        pD3DDevice = nullptr;
    }

    if (pD3D9) {
        pD3D9->Release();
        pD3D9 = nullptr;
    }
}

// Code from milkdropper, thank you!
void ToggleStretch(HWND hwnd) {
    if (!stretch) {
        int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int top = GetSystemMetrics(SM_YVIRTUALSCREEN);

        if (!fullscreen) {
            lastWindowStyle = GetWindowLong(hwnd, GWL_STYLE);
            lastWindowStyleEx = GetWindowLongW(hwnd, GWL_EXSTYLE);
            lastWindowStyleEx &= ~WS_EX_TOPMOST;
            GetWindowRect(hwnd, &lastRect);
        }

        d3dPp.BackBufferWidth = width;
        d3dPp.BackBufferHeight = height;

        pD3DDevice->Reset(&d3dPp);
        stretch = false;
        SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongW(hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);
        SetWindowPos(hwnd, HWND_NOTOPMOST, left, top, width, height, SWP_DRAWFRAME | SWP_FRAMECHANGED);

        stretch = true;
    } else {
        ShowCursor(TRUE);

        int width = lastRect.right - lastRect.left;
        int height = lastRect.bottom - lastRect.top;

        d3dPp.BackBufferWidth = width;
        d3dPp.BackBufferHeight = height;

        pD3DDevice->Reset(&d3dPp);
        stretch = false;

        SetWindowLongW(hwnd, GWL_STYLE, lastWindowStyle);
        SetWindowLongW(hwnd, GWL_EXSTYLE, lastWindowStyleEx);
        SetWindowPos(hwnd, HWND_NOTOPMOST, lastRect.left, lastRect.top, width, height, SWP_DRAWFRAME | SWP_FRAMECHANGED);
        SetWindowPos(hwnd, 0, 0, 0, 0, 0, SWP_DRAWFRAME | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
    }
    fullscreen = false;
}

void ToggleFullScreen(HWND hwnd) {
    if (!fullscreen) {
        ShowCursor(FALSE);

        if (!stretch) {
            lastWindowStyle = GetWindowLong(hwnd, GWL_STYLE);
            lastWindowStyleEx = GetWindowLongW(hwnd, GWL_EXSTYLE);
            lastWindowStyleEx &= ~WS_EX_TOPMOST;
            GetWindowRect(hwnd, &lastRect);
        }

        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);

        MONITORINFO info;
        info.cbSize = sizeof(MONITORINFO);

        GetMonitorInfoW(monitor, &info);

        int width = info.rcMonitor.right - info.rcMonitor.left;
        int height = info.rcMonitor.bottom - info.rcMonitor.top;

        SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongW(hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);
        SetWindowPos(hwnd, HWND_TOPMOST, info.rcMonitor.left, info.rcMonitor.top, width, height, SWP_DRAWFRAME | SWP_FRAMECHANGED);

        d3dPp.BackBufferWidth = width;
        d3dPp.BackBufferHeight = height;

        pD3DDevice->Reset(&d3dPp);
        fullscreen = true;
    }
    else {
        ShowCursor(TRUE);

        int width = lastRect.right - lastRect.left;
        int height = lastRect.bottom - lastRect.top;

        d3dPp.BackBufferWidth = width;
        d3dPp.BackBufferHeight = height;

        pD3DDevice->Reset(&d3dPp);
        fullscreen = false;

        SetWindowLongW(hwnd, GWL_STYLE, lastWindowStyle);
        SetWindowLongW(hwnd, GWL_EXSTYLE, lastWindowStyleEx);
        SetWindowPos(hwnd, HWND_NOTOPMOST, lastRect.left, lastRect.top, width, height, SWP_DRAWFRAME | SWP_FRAMECHANGED);
        SetWindowPos(hwnd, 0, 0, 0, 0, 0, SWP_DRAWFRAME | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
    }
     stretch = false;
}

// Opens a finished render in the built-in player. Separate from the render flow
// on purpose: people want to re-watch yesterday's output without rendering again.
static void PlayAVideoFile(HWND hwnd) {
    wchar_t path[MAX_PATH];
    path[0] = 0;

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd;
    ofn.lpstrFilter = L"Video files\0*.mp4;*.mkv;*.mov;*.m4v\0All files\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = ARRAYSIZE(path);
    ofn.lpstrTitle  = L"Play a video";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;

    if (!GetOpenFileNameW(&ofn))
        return;

    std::wstring error;
    if (!offline::PlayFile(api_orig_hinstance, path, 60, 1, error))
        MessageBoxW(hwnd, error.c_str(), L"Milk Run", MB_OK | MB_ICONWARNING);
}

// Ctrl+R renders the running preset to a file. The offline job drives this same
// CPlugin and the same D3D globals the live session holds, so the two can never
// overlap. The settings dialog runs first, while the session is merely paused;
// only a confirmed render is worth tearing the session down and rebuilding it for.
static void RenderCurrentPresetToVideo(HWND hwnd) {
    offline::RenderJobConfig cfg = offline::LastRenderConfig();
    if (!offline::ShowRenderSettingsDialog(api_orig_hinstance, hwnd,
                                           g_plugin.m_szCurrentPresetFile, cfg))
        return;

    // The render replaces d3dPp with its own size, so read the session's first.
    const int width  = (int)d3dPp.BackBufferWidth;
    const int height = (int)d3dPp.BackBufferHeight;

    // Deliberately not MyWriteConfig: the session is coming straight back, and a
    // render must not rewrite milk2.ini.
    g_plugin.PluginQuit();
    DeinitD3d();

    offline::RunRenderWithProgress(api_orig_hinstance, hwnd, cfg);

    g_plugin.PluginPreInitialize(0, 0);
    if (!InitD3d(hwnd, width, height, false)) {
        PostQuitMessage(0);
        return;
    }
    g_plugin.PluginInitialize(pD3DDevice, &d3dPp, hwnd, width, height);
}
LRESULT CALLBACK StaticWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch(uMsg) {
        
        //BeatDrop2077 DoubleClick = fullscreen on/off
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case app::ControlBar::CmdRender:
                RenderCurrentPresetToVideo(hWnd);
                return 0;
            case app::ControlBar::CmdSettings:
                app::ShowSettingsDialog(api_orig_hinstance, hWnd);
                return 0;
            case app::ControlBar::CmdPresets:
                // The preset browser the visualizer already draws. Reaching it by
                // a button rather than by knowing to press L is the point here.
                g_plugin.PluginShellWindowProc(hWnd, WM_KEYDOWN, 'L', 0);
                g_plugin.PluginShellWindowProc(hWnd, WM_CHAR, 'l', 0);
                return 0;
            case app::ControlBar::CmdPlay:
                PlayAVideoFile(hWnd);
                return 0;
            case app::ControlBar::CmdHelp:
                g_plugin.PluginShellWindowProc(hWnd, WM_KEYDOWN, VK_F1, 0);
                return 0;
            }
            break;

        case WM_MOVE:
        case WM_SIZE:
            g_controlBar.Reposition();
            break;

        case WM_MOUSEMOVE:
            g_controlBar.NotifyMouseActivity();
            break;

	    case WM_LBUTTONDBLCLK:
		ToggleFullScreen(hWnd);
		break;    
            
            
        case WM_CLOSE: {
            DestroyWindow( hWnd );
            UnregisterClassW(L"Direct3DWindowClass", NULL);
            return 0;
        }

        case WM_DESTROY: {
            g_controlBar.Destroy();
            PostQuitMessage(0);
            break;
        }

        case WM_KEYDOWN: {
            // MilkDrop's own 'r' binding arrives as WM_CHAR, and with Ctrl held that
            // character is 0x12 rather than 'r', so Ctrl+R does not collide.
            if (wParam == 'R' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                RenderCurrentPresetToVideo(hWnd);
                return 0;
            }

            // Ctrl+, opens app settings, the usual convention. Neither this nor
            // Ctrl+R collides with MilkDrop's own letter bindings: those arrive as
            // WM_CHAR, where Ctrl turns the character into a control code.
            if (wParam == VK_OEM_COMMA && (GetKeyState(VK_CONTROL) & 0x8000)) {
                app::ShowSettingsDialog(api_orig_hinstance, hWnd);
                return 0;
            }
            /*if (playback && wParam >= VK_F1 && wParam <= VK_F8) {
                switch (wParam) {
                case VK_F1:
                    playback->PauseOrResume();
                    break;
                case VK_F2:
                    playback->Stop();
                    break;
                case VK_F3:
                    playback->Previous();
                    break;
                case VK_F4:
                    playback->Next();
                    break;
                case VK_F5:
                    playback->SetVolume(playback->GetVolume() - 0.05);
                    break;
                case VK_F6:
                    playback->SetVolume(playback->GetVolume() + 0.05);
                    break;
                case VK_F7:
                    playback->ToggleShuffle();
                    break;
                case VK_F8:
                    playback->ToggleMute();
                    break;
                }
            }*/
            g_plugin.PluginShellWindowProc(hWnd, uMsg, wParam, lParam);
        }
        break;

        case WM_SYSKEYDOWN: {
            if (wParam == VK_F4) {
                PostQuitMessage(0);
            }
             else if (wParam == VK_SHIFT)
            {
                ToggleStretch(hWnd);
            }
            else if (wParam == VK_RETURN) {
                ToggleFullScreen(hWnd);
            }
            else {
                g_plugin.PluginShellWindowProc(hWnd, uMsg, wParam, lParam);
            }
            break;
        }

        default:
            return g_plugin.PluginShellWindowProc(hWnd, uMsg, wParam, lParam);
    }

    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

void RenderFrame() {
    {
        std::unique_lock<std::mutex> lock(pcmMutex);
        memcpy(pcmLeftOut, pcmLeftIn, SAMPLE_SIZE);
        memcpy(pcmRightOut, pcmRightIn, SAMPLE_SIZE);
        memset(pcmLeftIn, 0, SAMPLE_SIZE);
        memset(pcmRightIn, 0, SAMPLE_SIZE);
    }

    g_plugin.PluginRender(
        (unsigned char*) pcmLeftOut,
        (unsigned char*) pcmRightOut);
}

unsigned __stdcall CreateWindowAndRun(void* data) {
    HINSTANCE instance = (HINSTANCE) data;

#ifdef DEBUG
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    _CrtSetBreakAlloc(60);
#endif

    // Register the windows class
    WNDCLASSW wndClass;
    wndClass.style = CS_DBLCLKS;
    wndClass.lpfnWndProc = StaticWndProc;
    wndClass.cbClsExtra = 0;
    wndClass.cbWndExtra = 0;
    wndClass.hInstance = instance;
    wndClass.hIcon = NULL;
    wndClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    wndClass.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wndClass.lpszMenuName = NULL;
    wndClass.lpszClassName = L"Direct3DWindowClass";

    // SPOUT
	// Set Per Monitor awareness
    SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
    
    if (!RegisterClassW(&wndClass)) {
        DWORD dwError = GetLastError();
        if (dwError != ERROR_CLASS_ALREADY_EXISTS) {
            return 0;
        }
    }

    // SPOUT
	// make the window a fixed size to and avoid
	// resolution change for move instead of size
    int windowWidth = 800;
    int windowHeight = 800;

    RECT rc;
    SetRect(&rc, 0, 0, windowWidth, windowHeight);
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, false);
    
    // SPOUT
	// Centre on the desktop work area
	int WindowPosLeft = 0;
	int WindowPosTop = 0;
	RECT WorkArea;
	SystemParametersInfo(SPI_GETWORKAREA, 0, (LPVOID)&WorkArea, 0);
	WindowPosLeft += ((WorkArea.right - WorkArea.left) - windowWidth) / 2;
	WindowPosTop += ((WorkArea.bottom - WorkArea.top) - windowHeight) / 2;

	// SPOUT
	// Remove minimize and maximize
	DWORD dwStyle = (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME);

    // Create the render window
    HWND hwnd = CreateWindowW(
        L"Direct3DWindowClass",
        L"Milk Run",
        // WS_OVERLAPPEDWINDOW, // SPOUT
		dwStyle,
		WindowPosLeft, // SPOUT
		WindowPosTop,
		// CW_USEDEFAULT,
		// CW_USEDEFAULT,
        (rc.right - rc.left),
        (rc.bottom - rc.top),
        0,
        NULL,
        instance,
        0);

    if (!hwnd) {
        DWORD dwError = GetLastError();
        return 0;
    }

    if (!icon) {
        icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));
    }

    SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM) icon);
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM) icon);

    ShowWindow(hwnd, SW_SHOW);

   	// SPOUT
	// Make output resolution independent of the window size
	// The user can adjust this subsequently by resizing the BeatBox window
	// int lastWidth = windowWidth;
	// int lastHeight = windowHeight;
	int resolutionWidth = 800;
	int resolutionHeight = 800;

    g_plugin.PluginPreInitialize(0, 0);
    // InitD3d(hwnd, windowWidth, windowHeight);
	if (!InitD3d(hwnd, resolutionWidth, resolutionHeight, false))
		return 0;

    g_plugin.PluginInitialize(
        pD3DDevice,
        &d3dPp,
        hwnd,
        // windowWidth,
        // windowHeight);
		resolutionWidth,
		resolutionHeight);

    // After PluginInitialize on purpose: accepting loads a preset, which needs the
    // device. PluginInitialize has already run LoadRandomPreset against the empty
    // folder by now, so the dialog clears the error toast that left behind.
    if (app::FirstRunNeeded())
        app::ShowFirstRunDialog(instance, hwnd);

    // Buttons rather than hidden chords: nothing about this app announces that a
    // render is a keystroke away, and a visualizer window has no other chrome.
    g_controlBar.Create(instance, hwnd);

    MSG msg;
    msg.message = WM_NULL;

    PeekMessage(&msg, NULL, 0U, 0U, PM_NOREMOVE);
    while (WM_QUIT != msg.message) {
        if (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE) != 0) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else {
            GetAudioBuf(pcmLeftIn, pcmRightIn, SAMPLE_SIZE);
            RenderFrame();
        }
    }

    g_plugin.MyWriteConfig();
    g_plugin.PluginQuit();

    DeinitD3d();

    thread = nullptr;
    threadId = 0;

    return 1;
}

void StartRenderThread(HINSTANCE instance) {
    thread = (HANDLE) _beginthreadex(
        nullptr,
        0,
        &CreateWindowAndRun,
        (void *) instance,
        0,
        &threadId);
}

int StartThreads(HINSTANCE instance) {

    HRESULT hr = S_OK;

    hr = CoInitialize(NULL);
    if (FAILED(hr)) {
        ERR(L"CoInitialize failed: hr = 0x%08x", hr);
        return -__LINE__;
    }
    CoUninitializeOnExit cuoe;

    // argc==1 No additional params. Output disabled.
    // argc==3 Two additional params. Output file enabled (32bit IEEE 754 FLOAT).
    // argc==4 Three additional params. Output file enabled (LITTLE ENDIAN PCM).
    int argc = 1;
    LPCWSTR argv[4] = { L"", L"--file", L"loopback-capture.wav", L"--int-16" };
    hr = S_OK;

    // parse command line
    CPrefs prefs(argc, argv, hr);
    if (FAILED(hr)) {
        ERR(L"CPrefs::CPrefs constructor failed: hr = 0x%08x", hr);
        return -__LINE__;
    }
    if (S_FALSE == hr) {
        // nothing to do
        return 0;
    }

    // create a "loopback capture has started" event
    HANDLE hStartedEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (NULL == hStartedEvent) {
        ERR(L"CreateEvent failed: last error is %u", GetLastError());
        return -__LINE__;
    }
    CloseHandleOnExit closeStartedEvent(hStartedEvent);

    // create a "stop capturing now" event
    HANDLE hStopEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (NULL == hStopEvent) {
        ERR(L"CreateEvent failed: last error is %u", GetLastError());
        return -__LINE__;
    }
    CloseHandleOnExit closeStopEvent(hStopEvent);

    // create arguments for loopback capture thread
    LoopbackCaptureThreadFunctionArguments threadArgs;
    threadArgs.hr = E_UNEXPECTED; // thread will overwrite this
    threadArgs.pMMDevice = prefs.m_pMMDevice;
    threadArgs.bInt16 = prefs.m_bInt16;
    threadArgs.hFile = prefs.m_hFile;
    threadArgs.hStartedEvent = hStartedEvent;
    threadArgs.hStopEvent = hStopEvent;
    threadArgs.nFrames = 0;

    HANDLE hThread = CreateThread(
        NULL, 0,
        LoopbackCaptureThreadFunction, &threadArgs,
        0, NULL
    );
    if (NULL == hThread) {
        ERR(L"CreateThread failed: last error is %u", GetLastError());
        return -__LINE__;
    }
    CloseHandleOnExit closeThread(hThread);

    // wait for either capture to start or the thread to end
    HANDLE waitArray[2] = { hStartedEvent, hThread };
    DWORD dwWaitResult;
    dwWaitResult = WaitForMultipleObjects(
        ARRAYSIZE(waitArray), waitArray,
        FALSE, INFINITE
    );

    if (WAIT_OBJECT_0 + 1 == dwWaitResult) {
        ERR(L"Thread aborted before starting to loopback capture: hr = 0x%08x", threadArgs.hr);
        return -__LINE__;
    }

    if (WAIT_OBJECT_0 != dwWaitResult) {
        ERR(L"Unexpected WaitForMultipleObjects return value %u", dwWaitResult);
        return -__LINE__;
    }

    // at this point capture is running
    // wait for the user to press a key or for capture to error out
    
    /*HANDLE thread =*/ StartRenderThread(instance);
    WaitForSingleObject(thread, INFINITE);

    //NEED TO STOP CAPTURE
    // at this point capture is running
    // wait for the user to press a key or for capture to error out
    {
        WaitForSingleObjectOnExit waitForThread(hThread);
        SetEventOnExit setStopEvent(hStopEvent);
        HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);

        if (INVALID_HANDLE_VALUE == hStdIn) {
            ERR(L"GetStdHandle returned INVALID_HANDLE_VALUE: last error is %u", GetLastError());
            return -__LINE__;
        }

        LOG(L"%s", L"Press Enter to quit...");

        HANDLE rhHandles[2] = { hThread, hStdIn };

        bool bKeepWaiting = true;
        while (bKeepWaiting) {

            dwWaitResult = WaitForMultipleObjects(2, rhHandles, FALSE, INFINITE);

            switch (dwWaitResult) {

            case WAIT_OBJECT_0: // hThread
                ERR(L"%s", L"The thread terminated early - something bad happened");
                bKeepWaiting = false;
                break;

            case WAIT_OBJECT_0 + 1: // hStdIn
                                    // see if any of them was an Enter key-up event
                /*INPUT_RECORD rInput[128];
                DWORD nEvents;
                if (!ReadConsoleInput(hStdIn, rInput, ARRAYSIZE(rInput), &nEvents)) {
                    ERR(L"ReadConsoleInput failed: last error is %u", GetLastError());
                    bKeepWaiting = false;
                }
                else {
                    for (DWORD i = 0; i < nEvents; i++) {
                        if (
                            KEY_EVENT == rInput[i].EventType &&
                            VK_RETURN == rInput[i].Event.KeyEvent.wVirtualKeyCode &&
                            !rInput[i].Event.KeyEvent.bKeyDown
                            ) {*/
                            LOG(L"%s", L"Stopping capture...");
                            bKeepWaiting = false;
                            break;
                /*        }
                    }
                    // if none of them were Enter key-up events,
                    // continue waiting
                }*/
                break;

            default:
                ERR(L"WaitForMultipleObjects returned unexpected value 0x%08x", dwWaitResult);
                bKeepWaiting = false;
                break;
            } // switch
        } // while
    } // naked scope

    // at this point the thread is definitely finished

    DWORD exitCode;
    if (!GetExitCodeThread(hThread, &exitCode)) {
        ERR(L"GetExitCodeThread failed: last error is %u", GetLastError());
        return -__LINE__;
    }

    if (0 != exitCode) {
        ERR(L"Loopback capture thread exit code is %u; expected 0", exitCode);
        return -__LINE__;
    }

    if (S_OK != threadArgs.hr) {
        ERR(L"Thread HRESULT is 0x%08x", threadArgs.hr);
        return -__LINE__;
    }

    if (NULL != prefs.m_szFilename) {
        // everything went well... fixup the fact chunk in the file
        MMRESULT result = mmioClose(prefs.m_hFile, 0);
        prefs.m_hFile = NULL;
        if (MMSYSERR_NOERROR != result) {
            ERR(L"mmioClose failed: MMSYSERR = %u", result);
            return -__LINE__;
        }

        // reopen the file in read/write mode
        MMIOINFO mi = { 0 };
        prefs.m_hFile = mmioOpenW(const_cast<LPWSTR>(prefs.m_szFilename), &mi, MMIO_READWRITE);
        if (NULL == prefs.m_hFile) {
            ERR(L"mmioOpen(\"%ls\", ...) failed. wErrorRet == %u", prefs.m_szFilename, mi.wErrorRet);
            return -__LINE__;
        }

        // descend into the RIFF/WAVE chunk
        MMCKINFO ckRIFF = { 0 };
        ckRIFF.ckid = MAKEFOURCC('W', 'A', 'V', 'E'); // this is right for mmioDescend
        result = mmioDescend(prefs.m_hFile, &ckRIFF, NULL, MMIO_FINDRIFF);
        if (MMSYSERR_NOERROR != result) {
            ERR(L"mmioDescend(\"WAVE\") failed: MMSYSERR = %u", result);
            return -__LINE__;
        }

        // descend into the fact chunk
        MMCKINFO ckFact = { 0 };
        ckFact.ckid = MAKEFOURCC('f', 'a', 'c', 't');
        result = mmioDescend(prefs.m_hFile, &ckFact, &ckRIFF, MMIO_FINDCHUNK);
        if (MMSYSERR_NOERROR != result) {
            ERR(L"mmioDescend(\"fact\") failed: MMSYSERR = %u", result);
            return -__LINE__;
        }

        // write the correct data to the fact chunk
        LONG lBytesWritten = mmioWrite(
            prefs.m_hFile,
            reinterpret_cast<PCHAR>(&threadArgs.nFrames),
            sizeof(threadArgs.nFrames)
        );
        if (lBytesWritten != sizeof(threadArgs.nFrames)) {
            ERR(L"Updating the fact chunk wrote %u bytes; expected %u", lBytesWritten, (UINT32)sizeof(threadArgs.nFrames));
            return -__LINE__;
        }

        // ascend out of the fact chunk
        result = mmioAscend(prefs.m_hFile, &ckFact, 0);
        if (MMSYSERR_NOERROR != result) {
            ERR(L"mmioAscend(\"fact\") failed: MMSYSERR = %u", result);
            return -__LINE__;
        }
    }

    // let prefs' destructor call mmioClose

    return 0;
}

#ifdef COMPILE_AS_DLL
    BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
        module = hModule;
        api_orig_hinstance = hModule;
        return true;
    }
#else
    int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR szCmdLine, int iCmdShow) {
        api_orig_hinstance = hInstance;

        // A render is a headless, self-contained job: it drives the same plugin
        // singleton the interactive path does, so the two never run together.
        int exitCode = 0;
        if (offline::TryRunCommandLine(hInstance, exitCode))
            return exitCode;

        return StartThreads(hInstance);
    }
#endif

static std::string title;
/*
class VisaulizerPlugin : public musik::core::sdk::IPlugin {
    public:
        virtual void Release() override { }
        virtual const char* Name() override { return "Milkdrop2 IPcmVisualizer, IPlaybackRemote"; }
        virtual const char* Version() override { return "0.5.3"; }
        virtual const char* Author() override { return "clangen"; }
        virtual const char* Guid() override { return "5533c371-ed2b-40cf-aabb-f897661aeec1"; }
        virtual bool Configurable() override { return false; }
        virtual void Configure() override { }
        virtual void Reload() override { }
        virtual int SdkVersion() override { return musik::core::sdk::SdkVersion; }
};

class Visualizer :
    public musik::core::sdk::IPcmVisualizer ,
    public musik::core::sdk::IPlaybackRemote {
        public:
            virtual const char* Name() override {
                return "Milkdrop2";
            }

            virtual void Release() override {
                this->Hide();
            }

            virtual void SetPlaybackService(musik::core::sdk::IPlaybackService* playback) override {
                g_plugin.playbackService = playback;
                ::playback = playback;
            }

            virtual void OnTrackChanged(musik::core::sdk::ITrack* track) override {
                if (track) {
                    char buffer[1024];
                    track->GetString("title", buffer, 1024);
                    g_plugin.emulatedWinampSongTitle = std::string(buffer);
                }
                else {
                    g_plugin.emulatedWinampSongTitle = "";
                }
            }

            virtual void OnPlaybackStateChanged(musik::core::sdk::PlaybackState state) override {

            }

            virtual void OnVolumeChanged(double volume) override {

            }

            virtual void OnModeChanged(musik::core::sdk::RepeatMode repeatMode, bool shuffled) override {

            }

            virtual void OnPlayQueueChanged() override {

            }

            virtual void OnPlaybackTimeChanged(double time) override {

            }

            virtual void Write(musik::core::sdk::IBuffer* buffer) override {
                if (Visible()) {
                    float* b = buffer->BufferPointer();

                    std::unique_lock<std::mutex> lock(pcmMutex);

                    int n = 0;
                    for (int i = 0; i < buffer->Samples(); i++, n++) {
                        int x = i * 2;
                        pcmLeftIn[n % SAMPLE_SIZE] = (unsigned char)(b[i + 0] * 255.0f);
                        pcmRightIn[n % SAMPLE_SIZE] = (unsigned char)(b[i + 1] * 255.0f);
                    }
                }
            }

            virtual void Show() override {
                if (!Visible()) {
                    StartRenderThread(module);
                }
            }

            virtual void Hide() override {
                if (Visible()) {
                    PostThreadMessage(threadId, WM_QUIT, 0, 0);
                    WaitForSingleObject(thread, INFINITE);
                }
            }

            virtual bool Visible() override {
                return thread.load() != nullptr;
            }
};

static VisaulizerPlugin visualizerPlugin;
static Visualizer visualizer;

extern "C" DLL_EXPORT musik::core::sdk::IPlugin* GetPlugin() {
    return &visualizerPlugin;
}

extern "C" DLL_EXPORT musik::core::sdk::IPcmVisualizer* GetPcmVisualizer() {
    return &visualizer;
}

extern "C" DLL_EXPORT musik::core::sdk::IPlaybackRemote* GetPlaybackRemote() {
    return &visualizer;
}
*/
#ifdef DEBUG
struct _DEBUG_STATE {
    _DEBUG_STATE() {
    }

    ~_DEBUG_STATE() {
        _CrtDumpMemoryLeaks();
    }
};

_DEBUG_STATE ds;
#endif
