#include "settings_dialog.h"
#include "settings_dialog_ids.h"

// Only for IDR_RENDER_DIALOG_MANIFEST. One activation context resource serves the
// whole app; a second copy of the same manifest would be dead weight in the binary.
#include "render_dialog_ids.h"

#include "../plugin.h"
#include "../utility.h"   // the GetPrivateProfile*/WritePrivateProfile* wrappers

#include <commctrl.h>
#include <uxtheme.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <d3d9.h>
#include <stdio.h>
#include <string>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "d3d9.lib")

extern CPlugin g_plugin;

namespace app {

namespace {

/////////////////////////////////////////////////////////////////////////////
// Compiled defaults
//
// These mirror CPlugin::OverrideDefaults and CPlugin::MyPreInitialize. They are
// what Reset restores, and what a missing .ini key falls back to, so the dialog
// never reports a value the app would not actually use.

const float kDefTimeBetweenPresets     = 16.0f;
const float kDefTimeBetweenPresetsRand = 10.0f;
const float kDefBlendAuto              = 2.7f;
const float kDefBlendUser              = 1.7f;
const bool  kDefLockAtStartup          = false;

const bool  kDefHardCuts               = false;   // m_bHardCutsDisabled starts true
const float kDefHardCutLoudness        = 2.5f;
const float kDefHardCutHalflife        = 60.0f;

const int   kDefMaxFps                 = 60;      // OverrideDefaults, not the shell's own
const bool  kDefSaveCpu                = false;   // OverrideDefaults sets m_save_cpu = 0

const int   kDefTexBits                = 8;
const int   kDefTexSize                = -1;      // match the window
const int   kDefMeshX                  = 64;
const int   kDefMeshY                  = 64;

const bool  kDefShowFps                = false;
const bool  kDefShowRating             = false;
const bool  kDefShowPresetInfo         = false;
const bool  kDefShowSongTitle          = false;
const bool  kDefSongTitleAnims         = true;

// m_nGridX/m_nGridY are clamped to these in MyReadConfig, so anything larger is
// silently reduced. Reject it here instead.
const int kMinMesh  = 8;

// -1 and -2 are MilkDrop's two "work it out" texture sizes; everything else is a
// literal pixel size.
const int kTexSizeMatchWindow = -1;
const int kTexSizeNearestPow2 = -2;

/////////////////////////////////////////////////////////////////////////////
// Pages

enum PageIndex
{
    kPagePresets = 0,
    kPageDisplay = 1,
    kPageOverlay = 2,
    kPageCount   = 3
};

struct PageDef
{
    int            templateId;
    const wchar_t* label;
};

const PageDef kPages[kPageCount] = {
    { IDD_SETTINGS_PRESETS, L"Presets"   },
    { IDD_SETTINGS_DISPLAY, L"Display"   },
    { IDD_SETTINGS_OVERLAY, L"On screen" },
};

const wchar_t kTexBitsHint[] =
    L"More bits mean less banding in gradients, at the cost of video memory and speed.";

const wchar_t kRestartNote[] =
    L"The adapter and everything under Canvas take effect the next time Milk Run starts.";

/////////////////////////////////////////////////////////////////////////////
// Process environment for the dialog
//
// Same three scopes the render dialogs use, and for the same reasons. They are
// deliberately duplicated rather than shared: render_dialog.cpp keeps them in an
// anonymous namespace, which is where they belong.

// The visualizer's window thread never initializes COM, and the folder picker
// wants an apartment. Only undoes what it actually did, so it is safe on a thread
// that is already initialized either way.
class ComScope
{
public:
    ComScope() : m_owned(false)
    {
        // S_FALSE means the thread was already initialized, and it still took a
        // reference, so that case has to be balanced too. Only RPC_E_CHANGED_MODE
        // (a failure) leaves nothing to undo.
        m_owned = SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED));
    }
    ~ComScope()
    {
        if (m_owned)
            CoUninitialize();
    }
private:
    bool m_owned;
};

// Version 6 common controls, for the tab control. The app embeds no application
// manifest, so the only way to reach them is an activation context built from our
// own resource. A failure here costs the modern look and nothing else.
class ThemeScope
{
public:
    explicit ThemeScope(HINSTANCE hInstance)
        : m_ctx(INVALID_HANDLE_VALUE), m_cookie(0), m_active(false)
    {
        ACTCTXW desc;
        ZeroMemory(&desc, sizeof(desc));
        desc.cbSize         = sizeof(desc);
        desc.dwFlags        = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
        desc.hModule        = hInstance;
        desc.lpResourceName = MAKEINTRESOURCEW(IDR_RENDER_DIALOG_MANIFEST);

        m_ctx = CreateActCtxW(&desc);
        if (m_ctx != INVALID_HANDLE_VALUE)
            m_active = ActivateActCtx(m_ctx, &m_cookie) != FALSE;

        // Has to happen inside the context, otherwise the tab control class comes
        // from the version 5 library and stays unthemed.
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof(icc);
        icc.dwICC  = ICC_TAB_CLASSES | ICC_STANDARD_CLASSES;
        InitCommonControlsEx(&icc);
    }

    ~ThemeScope()
    {
        if (m_active)
            DeactivateActCtx(0, m_cookie);
        if (m_ctx != INVALID_HANDLE_VALUE)
            ReleaseActCtx(m_ctx);
    }

private:
    HANDLE     m_ctx;
    ULONG_PTR  m_cookie;
    bool       m_active;
};

// Per-monitor v2 is the only awareness mode where USER32 scales a dialog and its
// controls for the monitor it lands on. The app declares per-monitor v1, which
// does not, so this raises it for the dialog's thread and puts it back after.
//
// -4 is DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, resolved at run time so an
// older Windows gets an unscaled dialog instead of failing to start at all.
class DpiScope
{
public:
    DpiScope() : m_previous(NULL), m_set(NULL)
    {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (!user32)
            return;

        m_set = (SetContextFn)GetProcAddress(user32, "SetThreadDpiAwarenessContext");
        if (m_set)
            m_previous = m_set((void*)-4);
    }
    ~DpiScope()
    {
        if (m_set && m_previous)
            m_set(m_previous);
    }
private:
    typedef void* (WINAPI *SetContextFn)(void*);
    void*        m_previous;
    SetContextFn m_set;
};

/////////////////////////////////////////////////////////////////////////////
// Small text helpers

std::wstring Trim(const std::wstring& s)
{
    size_t first = 0;
    size_t last  = s.size();
    while (first < last && (s[first] == L' ' || s[first] == L'\t' || s[first] == L'"'))
        first++;
    while (last > first && (s[last - 1] == L' ' || s[last - 1] == L'\t' || s[last - 1] == L'"'))
        last--;
    return s.substr(first, last - first);
}

std::wstring GetText(HWND hDlg, int id)
{
    HWND h = GetDlgItem(hDlg, id);
    if (!h)
        return std::wstring();

    const int n = GetWindowTextLengthW(h);
    if (n <= 0)
        return std::wstring();

    std::wstring s((size_t)n + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize((size_t)n);
    return s;
}

void SetText(HWND hDlg, int id, const wchar_t* text)
{
    SetDlgItemTextW(hDlg, id, text);
}

// An edit box shows the head of its text, which for a path is the drive letter.
// The folder name is the part worth seeing, so scroll to it.
void SetPathText(HWND hDlg, int id, const wchar_t* text)
{
    SetDlgItemTextW(hDlg, id, text);

    const int end = lstrlenW(text);
    SendDlgItemMessageW(hDlg, id, EM_SETSEL, end, end);
    SendDlgItemMessageW(hDlg, id, EM_SCROLLCARET, 0, 0);
}

// Dialogs get no icon of their own, so they show the generic one in the taskbar
// and in Alt-Tab unless they take the visualizer's.
void InheritOwnerIcon(HWND hDlg)
{
    HWND owner = GetWindow(hDlg, GW_OWNER);
    if (!owner)
        return;

    HICON large = (HICON)SendMessageW(owner, WM_GETICON, ICON_BIG, 0);
    HICON tiny  = (HICON)SendMessageW(owner, WM_GETICON, ICON_SMALL, 0);

    if (large) SendMessageW(hDlg, WM_SETICON, ICON_BIG,   (LPARAM)large);
    if (tiny)  SendMessageW(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)tiny);
}

std::wstring Format(const wchar_t* fmt, ...)
{
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, fmt, args);
    va_end(args);
    return buf;
}

bool IsNumber(const std::wstring& s)
{
    if (s.empty())
        return false;

    int dots = 0;
    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] == L'.')
        {
            if (++dots > 1)
                return false;
        }
        else if (s[i] < L'0' || s[i] > L'9')
        {
            return false;
        }
    }
    return true;
}

// Every number in this dialog is a non-negative time or threshold, so a leading
// sign is not accepted: it would only ever be a typo.
bool ParseFloat(const std::wstring& text, float& out)
{
    const std::wstring s = Trim(text);
    if (!IsNumber(s))
        return false;
    out = (float)_wtof(s.c_str());
    return true;
}

// The ini stores floats with "%f", so reading one back and showing it verbatim
// gives "16.000000". %g drops the noise without rounding anything the user typed.
std::wstring FloatText(float v)
{
    return Format(L"%g", (double)v);
}

/////////////////////////////////////////////////////////////////////////////
// Combo boxes carrying an int
//
// Every value combo here maps a label to a number, and every one of them can meet
// a number that is not on its list, because milk2.ini is a text file somebody can
// edit. None of them silently snap to the nearest entry.

void AddIntItem(HWND hPage, int id, const wchar_t* label, int value)
{
    const LRESULT index = SendDlgItemMessageW(hPage, id, CB_ADDSTRING, 0, (LPARAM)label);
    if (index >= 0)
        SendDlgItemMessageW(hPage, id, CB_SETITEMDATA, (WPARAM)index, (LPARAM)value);
}

bool SelectIntItem(HWND hPage, int id, int value)
{
    const LRESULT count = SendDlgItemMessageW(hPage, id, CB_GETCOUNT, 0, 0);
    for (LRESULT i = 0; i < count; i++)
    {
        if ((int)SendDlgItemMessageW(hPage, id, CB_GETITEMDATA, (WPARAM)i, 0) == value)
        {
            SendDlgItemMessageW(hPage, id, CB_SETCURSEL, (WPARAM)i, 0);
            return true;
        }
    }
    return false;
}

// Keeps a value the dialog does not offer visible and selectable, rather than
// quietly replacing it the moment the dialog opens.
void SelectOrAddIntItem(HWND hPage, int id, int value, const wchar_t* strayFormat)
{
    if (SelectIntItem(hPage, id, value))
        return;

    AddIntItem(hPage, id, Format(strayFormat, value).c_str(), value);
    SelectIntItem(hPage, id, value);
}

int GetIntItem(HWND hPage, int id, int fallback)
{
    const LRESULT sel = SendDlgItemMessageW(hPage, id, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR)
        return fallback;
    return (int)SendDlgItemMessageW(hPage, id, CB_GETITEMDATA, (WPARAM)sel, 0);
}

/////////////////////////////////////////////////////////////////////////////
// Paths

bool DirectoryExists(const std::wstring& path)
{
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// The whole visualizer builds preset paths by concatenation, so a directory here
// without its trailing backslash produces filenames one character short of real.
std::wstring WithTrailingSlash(const std::wstring& path)
{
    if (path.empty() || path[path.size() - 1] == L'\\')
        return path;
    return path + L"\\";
}

bool BrowseForFolder(HWND parent, const std::wstring& start, std::wstring& chosen)
{
    IFileDialog* dlg = NULL;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                  IID_IFileDialog, (void**)&dlg);
    if (FAILED(hr))
    {
        MessageBoxW(parent,
                    Format(L"The folder picker could not be created (0x%08X). Type the "
                           L"path into the box instead.", (unsigned)hr).c_str(),
                    L"Cannot browse", MB_OK | MB_ICONWARNING);
        return false;
    }

    DWORD options = 0;
    if (SUCCEEDED(dlg->GetOptions(&options)))
        dlg->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);

    dlg->SetTitle(L"Choose the preset folder");

    if (!start.empty())
    {
        IShellItem* item = NULL;
        if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), NULL, IID_IShellItem,
                                                  (void**)&item)))
        {
            dlg->SetFolder(item);
            item->Release();
        }
    }

    // The visualizer resolves its preset and texture folders against the working
    // directory, so nothing the shell does here is allowed to move it.
    wchar_t szCwd[MAX_PATH];
    const DWORD cwdLen = GetCurrentDirectoryW(ARRAYSIZE(szCwd), szCwd);

    hr = dlg->Show(parent);

    if (cwdLen > 0 && cwdLen < ARRAYSIZE(szCwd))
        SetCurrentDirectoryW(szCwd);

    bool picked = false;
    if (SUCCEEDED(hr))
    {
        IShellItem* item = NULL;
        if (SUCCEEDED(dlg->GetResult(&item)))
        {
            wchar_t* path = NULL;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
            {
                chosen = path;
                CoTaskMemFree(path);
                picked = true;
            }
            item->Release();
        }
    }

    dlg->Release();
    return picked;
}

/////////////////////////////////////////////////////////////////////////////
// The settings themselves

struct AppSettings
{
    std::wstring presetDir;
    bool  lockAtStartup;

    float timeBetweenPresets;
    float timeBetweenPresetsRand;
    float blendAuto;
    float blendUser;

    bool  hardCuts;              // the inverse of m_bHardCutsDisabled
    float hardCutLoudness;
    float hardCutHalflife;

    UINT  adapterId;
    int   maxFps;                // 0 means unlimited
    bool  saveCpu;

    int   texBitsPerCh;
    int   texSize;               // -1 window, -2 nearest power of two, else pixels
    int   meshX;
    int   meshY;

    bool  showFps;
    bool  showRating;
    bool  showPresetInfo;
    bool  showSongTitle;
    bool  songTitleAnims;
};

// The folder MyPreInitialize would build, which is what Reset has to restore. It
// is derived from the executable's location, so it cannot be a compile-time
// constant the way the rest of the defaults are.
std::wstring DefaultPresetDir()
{
    return Format(L"%spresets\\", g_plugin.m_szMilkdrop2Path);
}

AppSettings Defaults()
{
    AppSettings s;
    s.presetDir              = DefaultPresetDir();
    s.lockAtStartup          = kDefLockAtStartup;
    s.timeBetweenPresets     = kDefTimeBetweenPresets;
    s.timeBetweenPresetsRand = kDefTimeBetweenPresetsRand;
    s.blendAuto              = kDefBlendAuto;
    s.blendUser              = kDefBlendUser;
    s.hardCuts               = kDefHardCuts;
    s.hardCutLoudness        = kDefHardCutLoudness;
    s.hardCutHalflife        = kDefHardCutHalflife;
    s.adapterId              = 0;
    s.maxFps                 = kDefMaxFps;
    s.saveCpu                = kDefSaveCpu;
    s.texBitsPerCh           = kDefTexBits;
    s.texSize                = kDefTexSize;
    s.meshX                  = kDefMeshX;
    s.meshY                  = kDefMeshY;
    s.showFps                = kDefShowFps;
    s.showRating             = kDefShowRating;
    s.showPresetInfo         = kDefShowPresetInfo;
    s.showSongTitle          = kDefShowSongTitle;
    s.songTitleAnims         = kDefSongTitleAnims;
    return s;
}

// Where each setting comes from is not uniform, and the split is deliberate:
//
//   - CPlugin's own members are public and live. The preset folder in particular
//     can change mid-session through the in-app load menu, so the member is the
//     only place with the current answer.
//   - m_max_fps_w and m_save_cpu are protected in CPluginShell, so the ini is the
//     only reachable copy. Reading it with the compiled default as the fallback
//     reproduces exactly what the shell itself would be holding.
//   - m_nTexSizeX and m_nTexSizeY are overwritten during device init with the
//     resolved pixel size, so the member says what the canvas is, not what the
//     user asked for. The ini key is the setting.
AppSettings ReadCurrent()
{
    AppSettings s = Defaults();
    wchar_t* pIni = g_plugin.GetConfigIniFile();

    s.presetDir              = g_plugin.m_szPresetDir;
    s.lockAtStartup          = g_plugin.m_bPresetLockOnAtStartup;

    s.timeBetweenPresets     = g_plugin.m_fTimeBetweenPresets;
    s.timeBetweenPresetsRand = g_plugin.m_fTimeBetweenPresetsRand;
    s.blendAuto              = g_plugin.m_fBlendTimeAuto;
    s.blendUser              = g_plugin.m_fBlendTimeUser;

    s.hardCuts               = !g_plugin.m_bHardCutsDisabled;
    s.hardCutLoudness        = g_plugin.m_fHardCutLoudnessThresh;
    s.hardCutHalflife        = g_plugin.m_fHardCutHalflife;

    s.adapterId              = g_plugin.m_adapterId;

    // CPluginShell::ReadConfig discards every key it owns unless the ini carries a
    // current version stamp, so an unstamped max_fps_w is a number the app is not
    // running with. Reporting it as the live setting would be a lie; the compiled
    // defaults Defaults() already put in s are the truth in that case.
    const int iniVer    = GetPrivateProfileIntW(L"settings", L"version",    -1, pIni);
    const int iniSubVer = GetPrivateProfileIntW(L"settings", L"subversion", -1, pIni);
    if (iniVer >= INT_VERSION && iniSubVer >= INT_SUBVERSION)
    {
        s.maxFps  = GetPrivateProfileIntW(L"settings", L"max_fps_w", kDefMaxFps, pIni);
        s.saveCpu = GetPrivateProfileBoolW(L"settings", L"save_cpu", kDefSaveCpu, pIni);
    }

    s.texBitsPerCh           = g_plugin.m_nTexBitsPerCh;

    // nTexSize and nMeshSize are the two keys MyWriteConfig has always written,
    // one number each for both axes. It writes them as the literals -1 and 64 on
    // the way out, so anything this dialog put in them would be gone by the next
    // start. The X/Y pairs are what it writes instead, and MyWriteConfig does not
    // touch those; the old keys seed them so an existing ini still means what it
    // meant. The pairs also match m_nTexSizeX/Y and m_nGridX/Y, which really are
    // two variables apiece.
    const int texLegacy      = GetPrivateProfileIntW(L"settings", L"nTexSize", kDefTexSize, pIni);
    s.texSize                = GetPrivateProfileIntW(L"settings", L"nTexSizeX", texLegacy, pIni);

    const int meshLegacy     = GetPrivateProfileIntW(L"settings", L"nMeshSize", kDefMeshX, pIni);
    s.meshX                  = GetPrivateProfileIntW(L"settings", L"nMeshSizeX", meshLegacy, pIni);
    s.meshY                  = GetPrivateProfileIntW(L"settings", L"nMeshSizeY", meshLegacy, pIni);

    s.showFps                = g_plugin.m_bShowFPS;
    s.showRating             = g_plugin.m_bShowRating;
    s.showPresetInfo         = g_plugin.m_bShowPresetInfo;
    s.showSongTitle          = g_plugin.m_bShowSongTitle;
    s.songTitleAnims         = g_plugin.m_bSongTitleAnims;

    return s;
}

// Swaps the preset folder over and rescans it. Returns false with a reason when
// the new folder holds nothing to play, having put the old one back: this is the
// same guard the in-app load menu applies, and for the same reason. An empty
// preset list leaves the visualizer with nothing to draw.
bool ApplyPresetDir(const std::wstring& dir, std::wstring& problem)
{
    if (dir == g_plugin.m_szPresetDir)
        return true;

    wchar_t szOld[MAX_PATH];
    lstrcpynW(szOld, g_plugin.m_szPresetDir, MAX_PATH);
    lstrcpynW(g_plugin.m_szPresetDir, dir.c_str(), MAX_PATH);

    g_plugin.UpdatePresetList(false, true, false);

    if (g_plugin.m_nPresets - g_plugin.m_nDirs <= 0)
    {
        problem = L"No .milk presets in that folder:\n\n" + dir;
        lstrcpynW(g_plugin.m_szPresetDir, szOld, MAX_PATH);
        g_plugin.UpdatePresetList(false, true, false);
        return false;
    }

    // The running preset is no longer anywhere in the list, so its index is stale.
    g_plugin.m_nCurrentPreset = -1;

    WritePrivateProfileStringW(L"settings", L"szPresetDir", g_plugin.m_szPresetDir,
                               g_plugin.GetConfigIniFile());
    return true;
}

void WriteSettings(const AppSettings& s)
{
    wchar_t* pIni = g_plugin.GetConfigIniFile();

    g_plugin.m_bPresetLockOnAtStartup = s.lockAtStartup;
    g_plugin.m_fTimeBetweenPresets    = s.timeBetweenPresets;
    g_plugin.m_fTimeBetweenPresetsRand= s.timeBetweenPresetsRand;
    g_plugin.m_fBlendTimeAuto         = s.blendAuto;
    g_plugin.m_fBlendTimeUser         = s.blendUser;
    g_plugin.m_bHardCutsDisabled      = !s.hardCuts;
    g_plugin.m_fHardCutLoudnessThresh = s.hardCutLoudness;
    g_plugin.m_fHardCutHalflife       = s.hardCutHalflife;
    g_plugin.m_adapterId              = s.adapterId;
    g_plugin.m_bShowFPS               = s.showFps;
    g_plugin.m_bShowRating            = s.showRating;
    g_plugin.m_bShowPresetInfo        = s.showPresetInfo;
    g_plugin.m_bShowSongTitle         = s.showSongTitle;
    g_plugin.m_bSongTitleAnims        = s.songTitleAnims;

    // Not applied live: the canvas textures are already allocated at the old depth
    // and size, and reallocating them means tearing the D3D9 device down.
    g_plugin.m_nTexBitsPerCh          = s.texBitsPerCh;

    WritePrivateProfileIntW(s.lockAtStartup, L"bPresetLockOnAtStartup", pIni, L"settings");
    WritePrivateProfileFloatW(s.timeBetweenPresets,     L"fTimeBetweenPresets",     pIni, L"settings");
    WritePrivateProfileFloatW(s.timeBetweenPresetsRand, L"fTimeBetweenPresetsRand", pIni, L"settings");
    WritePrivateProfileFloatW(s.blendAuto,              L"fBlendTimeAuto",          pIni, L"settings");
    WritePrivateProfileFloatW(s.blendUser,              L"fBlendTimeUser",          pIni, L"settings");

    WritePrivateProfileIntW(!s.hardCuts, L"bHardCutsDisabled", pIni, L"settings");
    WritePrivateProfileFloatW(s.hardCutLoudness, L"fHardCutLoudnessThresh", pIni, L"settings");
    WritePrivateProfileFloatW(s.hardCutHalflife, L"fHardCutHalflife",       pIni, L"settings");

    WritePrivateProfileIntW((int)s.adapterId, L"nVideoAdapterIndex", pIni, L"settings");

    WritePrivateProfileIntW(s.maxFps,  L"max_fps_w", pIni, L"settings");
    WritePrivateProfileIntW(s.saveCpu, L"save_cpu",  pIni, L"settings");

    // The stamps ReadConfig gates on. Nothing else writes them: this fork quits
    // through MyWriteConfig alone, and CPluginShell::WriteConfig, which is where
    // they used to come from, has no caller left. Without this the two settings
    // above would be written and then discarded on every start.
    WritePrivateProfileIntW(INT_VERSION,    L"version",    pIni, L"settings");
    WritePrivateProfileIntW(INT_SUBVERSION, L"subversion", pIni, L"settings");

    WritePrivateProfileIntW(s.texBitsPerCh, L"nTexBitsPerCh", pIni, L"settings");

    // One combo drives both axes: -1 and -2 are "match the window" and "nearest
    // power of two", and neither has a sensible per-axis reading.
    WritePrivateProfileIntW(s.texSize, L"nTexSizeX", pIni, L"settings");
    WritePrivateProfileIntW(s.texSize, L"nTexSizeY", pIni, L"settings");

    WritePrivateProfileIntW(s.meshX, L"nMeshSizeX", pIni, L"settings");
    WritePrivateProfileIntW(s.meshY, L"nMeshSizeY", pIni, L"settings");

    WritePrivateProfileIntW(s.showFps,         L"bShowFPS",        pIni, L"settings");
    WritePrivateProfileIntW(s.showRating,      L"bShowRating",     pIni, L"settings");
    WritePrivateProfileIntW(s.showPresetInfo,  L"bShowPresetInfo", pIni, L"settings");
    WritePrivateProfileIntW(s.showSongTitle,   L"bShowSongTitle",  pIni, L"settings");
    WritePrivateProfileIntW(s.songTitleAnims,  L"bSongTitleAnims", pIni, L"settings");
}

/////////////////////////////////////////////////////////////////////////////
// Dialog state

struct SettingsUi
{
    HINSTANCE   hInstance;
    HWND        pages[kPageCount];
    AppSettings opened;      // the values the dialog started from, for the restart test

    // Set while the controls are being filled in. Every SetWindowText fires the
    // same EN_CHANGE a keystroke does, and that handler validates the whole
    // dialog, so without this a half-populated page reports its own empty fields.
    bool        loading;

    SettingsOutcome outcome;

    SettingsUi() : hInstance(NULL), loading(false)
    {
        for (int i = 0; i < kPageCount; i++)
            pages[i] = NULL;
    }
};

/////////////////////////////////////////////////////////////////////////////
// Adapters
//
// The stored setting is a bare adapter index, and InitD3d reads index 0 as "work
// it out", so 0 cannot be used to pin the first adapter. That is not something
// this dialog can fix without changing the ini format, so it says so on the entry
// rather than letting the user pick something that does not do what it says.

void FillAdapters(HWND hPage, UINT selected)
{
    AddIntItem(hPage, IDC_SD_ADAPTER, L"Automatic: prefer a real GPU", 0);

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d)
    {
        // Nothing to enumerate against, so say why the list is short rather than
        // showing an empty box that looks like the machine has no GPU.
        AddIntItem(hPage, IDC_SD_ADAPTER, L"Direct3D 9 is not available to list adapters", -1);
        SelectIntItem(hPage, IDC_SD_ADAPTER, 0);
        return;
    }

    const UINT count = d3d->GetAdapterCount();
    for (UINT i = 0; i < count; i++)
    {
        D3DADAPTER_IDENTIFIER9 id;
        ZeroMemory(&id, sizeof(id));
        if (FAILED(d3d->GetAdapterIdentifier(i, 0, &id)))
            continue;

        // Description is ANSI in the D3D9 header regardless of the app's charset.
        wchar_t desc[MAX_DEVICE_IDENTIFIER_STRING];
        if (MultiByteToWideChar(CP_ACP, 0, id.Description, -1, desc, ARRAYSIZE(desc)) == 0)
            lstrcpynW(desc, L"(unnamed adapter)", ARRAYSIZE(desc));

        const std::wstring label = (i == 0)
            ? Format(L"%u: %s  (same as Automatic)", i, desc)
            : Format(L"%u: %s", i, desc);

        AddIntItem(hPage, IDC_SD_ADAPTER, label.c_str(), (int)i);
    }
    d3d->Release();

    SelectOrAddIntItem(hPage, IDC_SD_ADAPTER, (int)selected,
                       L"%d: adapter not present right now");
}

/////////////////////////////////////////////////////////////////////////////
// Loading and reading the pages

void LoadPresetsPage(HWND hPage, const AppSettings& s)
{
    SendDlgItemMessageW(hPage, IDC_SD_PRESET_DIR, EM_SETLIMITTEXT, MAX_PATH - 1, 0);
    SetPathText(hPage, IDC_SD_PRESET_DIR, s.presetDir.c_str());
    CheckDlgButton(hPage, IDC_SD_LOCK_AT_STARTUP, s.lockAtStartup ? BST_CHECKED : BST_UNCHECKED);

    SetText(hPage, IDC_SD_TIME_BETWEEN, FloatText(s.timeBetweenPresets).c_str());
    SetText(hPage, IDC_SD_TIME_RAND,    FloatText(s.timeBetweenPresetsRand).c_str());
    SetText(hPage, IDC_SD_BLEND_AUTO,   FloatText(s.blendAuto).c_str());
    SetText(hPage, IDC_SD_BLEND_USER,   FloatText(s.blendUser).c_str());

    CheckDlgButton(hPage, IDC_SD_HARD_CUTS, s.hardCuts ? BST_CHECKED : BST_UNCHECKED);
    SetText(hPage, IDC_SD_HARD_THRESH,   FloatText(s.hardCutLoudness).c_str());
    SetText(hPage, IDC_SD_HARD_HALFLIFE, FloatText(s.hardCutHalflife).c_str());
}

void LoadDisplayPage(HWND hPage, const AppSettings& s)
{
    SendDlgItemMessageW(hPage, IDC_SD_ADAPTER,  CB_RESETCONTENT, 0, 0);
    SendDlgItemMessageW(hPage, IDC_SD_MAX_FPS,  CB_RESETCONTENT, 0, 0);
    SendDlgItemMessageW(hPage, IDC_SD_TEX_BITS, CB_RESETCONTENT, 0, 0);
    SendDlgItemMessageW(hPage, IDC_SD_TEX_SIZE, CB_RESETCONTENT, 0, 0);

    FillAdapters(hPage, s.adapterId);

    AddIntItem(hPage, IDC_SD_MAX_FPS, L"Unlimited", 0);
    static const int kFpsChoices[] = { 24, 30, 45, 60, 75, 90, 100, 120 };
    for (int i = 0; i < ARRAYSIZE(kFpsChoices); i++)
        AddIntItem(hPage, IDC_SD_MAX_FPS, Format(L"%d", kFpsChoices[i]).c_str(), kFpsChoices[i]);
    SelectOrAddIntItem(hPage, IDC_SD_MAX_FPS, s.maxFps, L"%d");

    CheckDlgButton(hPage, IDC_SD_SAVE_CPU, s.saveCpu ? BST_CHECKED : BST_UNCHECKED);

    AddIntItem(hPage, IDC_SD_TEX_BITS, L"8-bit (X8R8G8B8)",              8);
    AddIntItem(hPage, IDC_SD_TEX_BITS, L"10-bit (A2R10G10B10)",         10);
    AddIntItem(hPage, IDC_SD_TEX_BITS, L"16-bit float (A16B16G16R16F)", 16);
    SelectOrAddIntItem(hPage, IDC_SD_TEX_BITS, s.texBitsPerCh,
                       L"%d bits per channel, from milk2.ini");
    SetText(hPage, IDC_SD_TEX_BITS_HINT, kTexBitsHint);

    AddIntItem(hPage, IDC_SD_TEX_SIZE, L"Match the window",     kTexSizeMatchWindow);
    AddIntItem(hPage, IDC_SD_TEX_SIZE, L"Nearest power of two", kTexSizeNearestPow2);
    static const int kTexSizeChoices[] = { 256, 512, 1024, 2048, 4096 };
    for (int i = 0; i < ARRAYSIZE(kTexSizeChoices); i++)
        AddIntItem(hPage, IDC_SD_TEX_SIZE,
                   Format(L"%d x %d pixels", kTexSizeChoices[i], kTexSizeChoices[i]).c_str(),
                   kTexSizeChoices[i]);
    SelectOrAddIntItem(hPage, IDC_SD_TEX_SIZE, s.texSize, L"%d pixels, from milk2.ini");

    SetDlgItemInt(hPage, IDC_SD_MESH_X, s.meshX, FALSE);
    SetDlgItemInt(hPage, IDC_SD_MESH_Y, s.meshY, FALSE);

    SetText(hPage, IDC_SD_RESTART_NOTE, kRestartNote);
}

void LoadOverlayPage(HWND hPage, const AppSettings& s)
{
    CheckDlgButton(hPage, IDC_SD_SHOW_FPS,          s.showFps         ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hPage, IDC_SD_SHOW_RATING,       s.showRating      ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hPage, IDC_SD_SHOW_PRESET_INFO,  s.showPresetInfo  ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hPage, IDC_SD_SHOW_SONG_TITLE,   s.showSongTitle   ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hPage, IDC_SD_SONG_TITLE_ANIMS,  s.songTitleAnims  ? BST_CHECKED : BST_UNCHECKED);
}

void LoadPages(SettingsUi* ui, const AppSettings& s)
{
    ui->loading = true;
    LoadPresetsPage(ui->pages[kPagePresets], s);
    LoadDisplayPage(ui->pages[kPageDisplay], s);
    LoadOverlayPage(ui->pages[kPageOverlay], s);
    ui->loading = false;
}

// Reads every control. Numbers that will not parse come back as a negative marker
// rather than a substituted value, so validation reports them instead of the
// dialog quietly deciding what the user meant.
const float kUnparsed = -1.0f;

float ReadFloat(HWND hPage, int id)
{
    float v = kUnparsed;
    if (!ParseFloat(GetText(hPage, id), v))
        return kUnparsed;
    return v;
}

AppSettings ReadPages(SettingsUi* ui)
{
    AppSettings s;
    HWND presets = ui->pages[kPagePresets];
    HWND display = ui->pages[kPageDisplay];
    HWND overlay = ui->pages[kPageOverlay];

    s.presetDir              = Trim(GetText(presets, IDC_SD_PRESET_DIR));
    s.lockAtStartup          = IsDlgButtonChecked(presets, IDC_SD_LOCK_AT_STARTUP) == BST_CHECKED;

    s.timeBetweenPresets     = ReadFloat(presets, IDC_SD_TIME_BETWEEN);
    s.timeBetweenPresetsRand = ReadFloat(presets, IDC_SD_TIME_RAND);
    s.blendAuto              = ReadFloat(presets, IDC_SD_BLEND_AUTO);
    s.blendUser              = ReadFloat(presets, IDC_SD_BLEND_USER);

    s.hardCuts               = IsDlgButtonChecked(presets, IDC_SD_HARD_CUTS) == BST_CHECKED;
    s.hardCutLoudness        = ReadFloat(presets, IDC_SD_HARD_THRESH);
    s.hardCutHalflife        = ReadFloat(presets, IDC_SD_HARD_HALFLIFE);

    s.adapterId              = (UINT)GetIntItem(display, IDC_SD_ADAPTER, 0);
    s.maxFps                 = GetIntItem(display, IDC_SD_MAX_FPS, kDefMaxFps);
    s.saveCpu                = IsDlgButtonChecked(display, IDC_SD_SAVE_CPU) == BST_CHECKED;

    s.texBitsPerCh           = GetIntItem(display, IDC_SD_TEX_BITS, kDefTexBits);
    s.texSize                = GetIntItem(display, IDC_SD_TEX_SIZE, kDefTexSize);
    s.meshX                  = (int)GetDlgItemInt(display, IDC_SD_MESH_X, NULL, FALSE);
    s.meshY                  = (int)GetDlgItemInt(display, IDC_SD_MESH_Y, NULL, FALSE);

    s.showFps                = IsDlgButtonChecked(overlay, IDC_SD_SHOW_FPS)         == BST_CHECKED;
    s.showRating             = IsDlgButtonChecked(overlay, IDC_SD_SHOW_RATING)      == BST_CHECKED;
    s.showPresetInfo         = IsDlgButtonChecked(overlay, IDC_SD_SHOW_PRESET_INFO) == BST_CHECKED;
    s.showSongTitle          = IsDlgButtonChecked(overlay, IDC_SD_SHOW_SONG_TITLE)  == BST_CHECKED;
    s.songTitleAnims         = IsDlgButtonChecked(overlay, IDC_SD_SONG_TITLE_ANIMS) == BST_CHECKED;

    return s;
}

/////////////////////////////////////////////////////////////////////////////
// Validation
//
// Every check names the page it lives on and the control that needs attention, so
// OK can put the user in front of the problem instead of describing it and
// leaving them to find it behind a tab.

struct Problem
{
    std::wstring text;
    int          page;
    int          controlId;

    Problem() : page(kPagePresets), controlId(0) {}
};

bool Validate(const AppSettings& s, Problem& p)
{
    if (s.presetDir.empty())
    {
        p.text = L"Choose the folder your .milk presets are in.";
        p.page = kPagePresets;  p.controlId = IDC_SD_PRESET_DIR;
        return false;
    }
    if (!DirectoryExists(WithTrailingSlash(s.presetDir)))
    {
        p.text = L"That preset folder does not exist.";
        p.page = kPagePresets;  p.controlId = IDC_SD_PRESET_DIR;
        return false;
    }

    // MyReadConfig floors this at 0.1, so anything below it is not the value that
    // would be used.
    if (s.timeBetweenPresets < 0.1f)
    {
        p.text = L"Seconds per preset must be a number, 0.1 or more.";
        p.page = kPagePresets;  p.controlId = IDC_SD_TIME_BETWEEN;
        return false;
    }
    if (s.timeBetweenPresetsRand < 0.0f)
    {
        p.text = L"The extra random time must be a number, 0 or more.";
        p.page = kPagePresets;  p.controlId = IDC_SD_TIME_RAND;
        return false;
    }
    if (s.blendAuto < 0.0f)
    {
        p.text = L"The automatic blend time must be a number, 0 or more.";
        p.page = kPagePresets;  p.controlId = IDC_SD_BLEND_AUTO;
        return false;
    }
    if (s.blendUser < 0.0f)
    {
        p.text = L"The by-hand blend time must be a number, 0 or more.";
        p.page = kPagePresets;  p.controlId = IDC_SD_BLEND_USER;
        return false;
    }

    if (s.hardCuts && s.hardCutLoudness <= 0.0f)
    {
        p.text = L"Hard cut loudness must be a number above 0. Around 2.5 is normal.";
        p.page = kPagePresets;  p.controlId = IDC_SD_HARD_THRESH;
        return false;
    }
    if (s.hardCuts && s.hardCutHalflife <= 0.0f)
    {
        p.text = L"The average seconds between hard cuts must be a number above 0.";
        p.page = kPagePresets;  p.controlId = IDC_SD_HARD_HALFLIFE;
        return false;
    }

    if (s.meshX < kMinMesh || s.meshX > MAX_GRID_X)
    {
        p.text = Format(L"Mesh width must be between %d and %d.", kMinMesh, MAX_GRID_X);
        p.page = kPageDisplay;  p.controlId = IDC_SD_MESH_X;
        return false;
    }
    if (s.meshY < kMinMesh || s.meshY > MAX_GRID_Y)
    {
        p.text = Format(L"Mesh height must be between %d and %d.", kMinMesh, MAX_GRID_Y);
        p.page = kPageDisplay;  p.controlId = IDC_SD_MESH_Y;
        return false;
    }

    if (s.adapterId == (UINT)-1)
    {
        p.text = L"Direct3D 9 could not be loaded, so no adapter can be chosen. "
                 L"Leave this on Automatic.";
        p.page = kPageDisplay;  p.controlId = IDC_SD_ADAPTER;
        return false;
    }

    p.text.clear();
    return true;
}

void ShowPage(HWND hDlg, SettingsUi* ui, int page)
{
    for (int i = 0; i < kPageCount; i++)
        ShowWindow(ui->pages[i], (i == page) ? SW_SHOW : SW_HIDE);

    TabCtrl_SetCurSel(GetDlgItem(hDlg, IDC_SD_TABS), page);
}

void UpdateEnabledState(SettingsUi* ui)
{
    HWND presets = ui->pages[kPagePresets];
    const bool hard = IsDlgButtonChecked(presets, IDC_SD_HARD_CUTS) == BST_CHECKED;

    const int hardControls[] = {
        IDC_SD_HARD_THRESH, IDC_SD_HARD_HALFLIFE,
        IDC_SD_HARD_THRESH_LABEL, IDC_SD_HARD_HALFLIFE_LABEL, IDC_SD_HARD_HINT
    };
    for (int i = 0; i < ARRAYSIZE(hardControls); i++)
        EnableWindow(GetDlgItem(presets, hardControls[i]), hard ? TRUE : FALSE);

    HWND overlay = ui->pages[kPageOverlay];
    const bool title = IsDlgButtonChecked(overlay, IDC_SD_SHOW_SONG_TITLE) == BST_CHECKED;
    EnableWindow(GetDlgItem(overlay, IDC_SD_SONG_TITLE_ANIMS), title ? TRUE : FALSE);
}

// The line under the tabs, kept current as the user types, so a bad number is
// visible before they reach for OK.
void UpdateWarning(HWND hDlg, SettingsUi* ui)
{
    if (ui->loading)
        return;

    Problem p;
    Validate(ReadPages(ui), p);
    SetText(hDlg, IDC_SD_WARNING, p.text.c_str());
}

/////////////////////////////////////////////////////////////////////////////
// Page window procedure
//
// One procedure for all three pages: the control ids are unique across them, and
// everything a page does with a change is to tell the frame to re-check itself.

INT_PTR CALLBACK PageProc(HWND hPage, UINT msg, WPARAM wp, LPARAM lp)
{
    SettingsUi* ui = (SettingsUi*)GetWindowLongPtrW(hPage, DWLP_USER);

    switch (msg)
    {
    case WM_INITDIALOG:
        SetWindowLongPtrW(hPage, DWLP_USER, (LONG_PTR)lp);

        // Without this the page paints its own dialog-face grey over the themed
        // tab body, and every group box and label on it sits in a grey rectangle.
        EnableThemeDialogTexture(hPage, ETDT_ENABLETAB);
        return TRUE;

    case WM_COMMAND:
    {
        if (!ui || ui->loading)
            return TRUE;

        HWND hDlg = GetParent(hPage);
        const int id   = LOWORD(wp);
        const int code = HIWORD(wp);

        switch (id)
        {
        case IDC_SD_PRESET_DIR_BROWSE:
        {
            std::wstring chosen;
            if (BrowseForFolder(hDlg, Trim(GetText(hPage, IDC_SD_PRESET_DIR)), chosen))
            {
                SetPathText(hPage, IDC_SD_PRESET_DIR, WithTrailingSlash(chosen).c_str());
                UpdateWarning(hDlg, ui);
            }
            return TRUE;
        }

        case IDC_SD_HARD_CUTS:
        case IDC_SD_SHOW_SONG_TITLE:
            UpdateEnabledState(ui);
            UpdateWarning(hDlg, ui);
            return TRUE;

        case IDOK:
        case IDCANCEL:
            // Enter and Escape belong to the frame, which owns the buttons. The
            // dialog manager sends them there and not here, so this only matters if
            // something ever routes one through a page: swallowing it would make OK
            // do nothing while focus sat on a page control.
            return FALSE;

        default:
            if (code == EN_CHANGE || code == CBN_SELCHANGE || code == BN_CLICKED)
                UpdateWarning(hDlg, ui);
            return TRUE;
        }
    }
    }

    return FALSE;
}

/////////////////////////////////////////////////////////////////////////////
// Frame window procedure

bool CreatePages(HWND hDlg, SettingsUi* ui)
{
    HWND tabs = GetDlgItem(hDlg, IDC_SD_TABS);
    if (!tabs)
        return false;

    // Spelled out rather than TabCtrl_InsertItem: the project does not define
    // UNICODE, so that macro resolves to TCM_INSERTITEMA, which reads a wide label
    // as ANSI and puts a one-character tab on screen. Every other tab message here
    // has no A/W split, so only this one needs saying.
    for (int i = 0; i < kPageCount; i++)
    {
        TCITEMW item;
        ZeroMemory(&item, sizeof(item));
        item.mask    = TCIF_TEXT;
        item.pszText = (LPWSTR)kPages[i].label;
        SendMessageW(tabs, TCM_INSERTITEMW, (WPARAM)i, (LPARAM)&item);
    }

    // The pages are children of the frame, not of the tab control, so the frame's
    // dialog manager can tab into them. DS_CONTROL on the templates is what gives
    // them WS_EX_CONTROLPARENT and makes that work.
    RECT display;
    GetWindowRect(tabs, &display);
    MapWindowPoints(NULL, hDlg, (LPPOINT)&display, 2);
    TabCtrl_AdjustRect(tabs, FALSE, &display);

    for (int i = 0; i < kPageCount; i++)
    {
        ui->pages[i] = CreateDialogParamW(ui->hInstance,
                                          MAKEINTRESOURCEW(kPages[i].templateId),
                                          hDlg, PageProc, (LPARAM)ui);
        if (!ui->pages[i])
            return false;

        SetWindowPos(ui->pages[i], HWND_TOP,
                     display.left, display.top,
                     display.right - display.left, display.bottom - display.top,
                     SWP_NOACTIVATE);
        ShowWindow(ui->pages[i], SW_HIDE);
    }

    return true;
}

INT_PTR CALLBACK SettingsProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    SettingsUi* ui = (SettingsUi*)GetWindowLongPtrW(hDlg, DWLP_USER);

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        ui = (SettingsUi*)lp;
        SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)ui);

        InheritOwnerIcon(hDlg);

        if (!CreatePages(hDlg, ui))
        {
            // Half a settings window is worse than none: the pages carry every
            // control, so there is nothing to fall back to.
            MessageBoxW(hDlg,
                        L"The settings pages could not be created. Their resources "
                        L"are missing from this build.",
                        L"Cannot open settings", MB_OK | MB_ICONERROR);
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }

        ui->opened = ReadCurrent();
        LoadPages(ui, ui->opened);
        UpdateEnabledState(ui);
        ShowPage(hDlg, ui, kPagePresets);
        UpdateWarning(hDlg, ui);
        return TRUE;
    }

    case WM_CTLCOLORSTATIC:
        if (GetDlgCtrlID((HWND)lp) == IDC_SD_WARNING)
        {
            SetTextColor((HDC)wp, RGB(176, 32, 32));
            SetBkMode((HDC)wp, TRANSPARENT);
            return (INT_PTR)GetSysColorBrush(COLOR_BTNFACE);
        }
        return FALSE;

    case WM_NOTIFY:
    {
        NMHDR* hdr = (NMHDR*)lp;
        if (ui && hdr && hdr->idFrom == IDC_SD_TABS && hdr->code == TCN_SELCHANGE)
        {
            ShowPage(hDlg, ui, TabCtrl_GetCurSel(hdr->hwndFrom));
            return TRUE;
        }
        return FALSE;
    }

    case WM_COMMAND:
    {
        if (!ui)
            return FALSE;

        switch (LOWORD(wp))
        {
        case IDC_SD_RESET:
        {
            // Only the controls: nothing reaches milk2.ini until OK, so this is
            // still undoable with Cancel.
            AppSettings d = Defaults();
            LoadPages(ui, d);
            UpdateEnabledState(ui);
            UpdateWarning(hDlg, ui);
            return TRUE;
        }

        case IDOK:
        {
            const AppSettings s = ReadPages(ui);

            Problem p;
            if (!Validate(s, p))
            {
                SetText(hDlg, IDC_SD_WARNING, p.text.c_str());
                ShowPage(hDlg, ui, p.page);
                MessageBoxW(hDlg, p.text.c_str(), L"Cannot save yet",
                            MB_OK | MB_ICONWARNING);
                if (p.controlId)
                    SetFocus(GetDlgItem(ui->pages[p.page], p.controlId));
                return TRUE;
            }

            const std::wstring dir = WithTrailingSlash(s.presetDir);

            std::wstring problem;
            if (!ApplyPresetDir(dir, problem))
            {
                SetText(hDlg, IDC_SD_WARNING, problem.c_str());
                ShowPage(hDlg, ui, kPagePresets);
                MessageBoxW(hDlg, problem.c_str(), L"Cannot save yet",
                            MB_OK | MB_ICONWARNING);
                SetFocus(GetDlgItem(ui->pages[kPagePresets], IDC_SD_PRESET_DIR));
                return TRUE;
            }

            WriteSettings(s);

            ui->outcome.accepted         = true;
            ui->outcome.presetDirChanged = (dir != ui->opened.presetDir);
            ui->outcome.restartRequired  =
                (s.adapterId    != ui->opened.adapterId)    ||
                (s.texBitsPerCh != ui->opened.texBitsPerCh) ||
                (s.texSize      != ui->opened.texSize)      ||
                (s.meshX        != ui->opened.meshX)        ||
                (s.meshY        != ui->opened.meshY)        ||
                (s.maxFps       != ui->opened.maxFps)       ||
                (s.saveCpu      != ui->opened.saveCpu);

            EndDialog(hDlg, IDOK);
            return TRUE;
        }

        case IDCANCEL:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    }

    case WM_DESTROY:
        if (ui)
        {
            for (int i = 0; i < kPageCount; i++)
            {
                if (ui->pages[i])
                {
                    DestroyWindow(ui->pages[i]);
                    ui->pages[i] = NULL;
                }
            }
        }
        return FALSE;
    }

    return FALSE;
}

} // namespace

SettingsOutcome ShowSettingsDialog(HINSTANCE hInstance, HWND parent)
{
    ComScope   com;
    ThemeScope theme(hInstance);
    DpiScope   dpi;

    SettingsUi ui;
    ui.hInstance = hInstance;

    const INT_PTR r = DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_SETTINGS),
                                      parent, SettingsProc, (LPARAM)&ui);
    if (r == -1)
    {
        MessageBoxW(parent,
                    L"The settings window could not be created. Its resources are "
                    L"missing from this build.",
                    L"Cannot open settings", MB_OK | MB_ICONERROR);
        return SettingsOutcome();
    }

    if (ui.outcome.accepted && ui.outcome.restartRequired)
    {
        MessageBoxW(parent,
                    L"Saved. The adapter, canvas and frame rate settings are read "
                    L"when Milk Run starts, so restart it to see them.",
                    L"Settings saved", MB_OK | MB_ICONINFORMATION);
    }

    return ui.outcome;
}

} // namespace app
