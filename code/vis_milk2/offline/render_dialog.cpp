#include "render_dialog.h"
#include "render_dialog_ids.h"
#include "render_job.h"
#include "audio_source.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <process.h>
#include <math.h>
#include <stdio.h>
#include <string>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

namespace offline {

namespace {

// Posted by the render thread once RunRenderJob has returned. The progress dialog
// stays up until it arrives, which is what makes Cancel cooperative rather than a
// race against a thread still holding the D3D device.
const UINT WM_RENDER_FINISHED = WM_APP + 1;

const UINT_PTR kProbeTimer = 1;   // settings: audio duration, after typing settles
const UINT_PTR kTickTimer  = 1;   // progress: repaint the counters

const int kProbeDelayMs = 500;
const int kTickMs       = 200;

// Quality is a constant-quality level where lower is better, so the slider runs
// backwards: left is a smaller file, right is a better picture.
const int kQualityWorst = 40;
const int kQualityBest  = 10;

const int kMaxDimension = 8192;   // NVENC HEVC tops out here

struct SizePreset
{
    int width;
    int height;
    const wchar_t* label;
};

const SizePreset kSizePresets[] = {
    { 1280,  720, L"1280 x 720"  },
    { 1920, 1080, L"1920 x 1080" },
    { 2560, 1440, L"2560 x 1440" },
    { 3840, 2160, L"3840 x 2160" },
};

const wchar_t* const kFpsPresets[] = {
    L"24", L"25", L"30", L"48", L"50", L"60", L"120"
};

// Order matches EncoderBackend.
const wchar_t* const kEncoderLabels[] = {
    L"Automatic",
    L"NVIDIA NVENC",
    L"Media Foundation"
};

/////////////////////////////////////////////////////////////////////////////
// Process environment for the dialogs

// The visualizer's window thread never initializes COM, and both the file dialogs
// and the Media Foundation duration probe want an apartment. Only undoes what it
// actually did, so it is safe on a thread that is already initialized either way.
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

// Version 6 common controls, for the trackbar and the progress bar. The app embeds
// no application manifest, so the only way to reach them is an activation context
// built from our own resource. A failure here costs the modern look and nothing
// else, so it is not worth refusing to render over.
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

        // Has to happen inside the context, otherwise the trackbar and progress bar
        // classes come from the version 5 library and stay unthemed.
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof(icc);
        icc.dwICC  = ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
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
// The file name is the part worth seeing, so scroll to it.
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

// 12345 -> "12,345". Long renders run to six figures, and an unbroken run of
// digits is the one number in this dialog nobody can read at a glance.
std::wstring WithThousands(long long v)
{
    wchar_t plain[32];
    swprintf_s(plain, L"%lld", v < 0 ? -v : v);

    std::wstring out;
    const int len = (int)wcslen(plain);
    for (int i = 0; i < len; i++)
    {
        if (i > 0 && ((len - i) % 3) == 0)
            out += L',';
        out += plain[i];
    }
    return (v < 0) ? (L"-" + out) : out;
}

// Seconds as a clock: 3:25, or 1:03:25 once it runs past an hour.
std::wstring Clock(double seconds, bool tenths = false)
{
    if (seconds < 0.0)
        seconds = 0.0;

    const int whole = (int)seconds;
    const int h = whole / 3600;
    const int m = (whole % 3600) / 60;
    const int s = whole % 60;
    const int t = (int)((seconds - whole) * 10.0);

    if (h > 0)
        return Format(L"%d:%02d:%02d", h, m, s);
    if (tenths)
        return Format(L"%d:%02d.%d", m, s, t);
    return Format(L"%d:%02d", m, s);
}

std::wstring FpsText(int num, int den)
{
    if (den == 1)
        return Format(L"%d", num);
    return Format(L"%.3f", (double)num / (double)den);
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

int Gcd(int a, int b)
{
    while (b)
    {
        const int t = a % b;
        a = b;
        b = t;
    }
    return a ? a : 1;
}

// Accepts "60", "24000/1001" and "29.97", the same three forms the command line
// takes, so a rate copied from one works in the other.
bool ParseFps(const std::wstring& text, int& num, int& den)
{
    const std::wstring s = Trim(text);
    if (s.empty())
        return false;

    const size_t slash = s.find(L'/');
    if (slash != std::wstring::npos)
    {
        const std::wstring a = Trim(s.substr(0, slash));
        const std::wstring b = Trim(s.substr(slash + 1));
        if (!IsNumber(a) || !IsNumber(b))
            return false;
        num = _wtoi(a.c_str());
        den = _wtoi(b.c_str());
    }
    else if (s.find(L'.') != std::wstring::npos)
    {
        if (!IsNumber(s))
            return false;
        num = (int)(_wtof(s.c_str()) * 1000.0 + 0.5);
        den = 1000;
    }
    else
    {
        if (!IsNumber(s))
            return false;
        num = _wtoi(s.c_str());
        den = 1;
    }

    if (num <= 0 || den <= 0)
        return false;

    const int g = Gcd(num, den);
    num /= g;
    den /= g;
    return true;
}

// Accepts "90", "1:30" and "1:02:03". An empty string is zero, not an error.
bool ParseTime(const std::wstring& text, double& out)
{
    const std::wstring s = Trim(text);
    out = 0.0;
    if (s.empty())
        return true;

    double parts[3] = { 0.0, 0.0, 0.0 };
    int count = 0;
    size_t start = 0;

    while (count < 3)
    {
        const size_t colon = s.find(L':', start);
        const std::wstring token = (colon == std::wstring::npos)
                                 ? s.substr(start)
                                 : s.substr(start, colon - start);
        if (!IsNumber(token))
            return false;

        parts[count++] = _wtof(token.c_str());

        if (colon == std::wstring::npos)
        {
            start = s.size();
            break;
        }
        start = colon + 1;
    }

    if (start < s.size())
        return false;   // more than hours:minutes:seconds

    for (int i = 0; i < count; i++)
        out = out * 60.0 + parts[i];
    return true;
}

/////////////////////////////////////////////////////////////////////////////
// Files

bool FileExists(const std::wstring& path)
{
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring DirectoryOf(const std::wstring& path)
{
    wchar_t buf[MAX_PATH];
    lstrcpynW(buf, path.c_str(), MAX_PATH);
    PathRemoveFileSpecW(buf);
    return buf;
}

// The output lands next to the song by default, same name with an .mp4 on it,
// because that is where someone looks for it afterwards.
std::wstring DefaultOutputFor(const std::wstring& audioPath)
{
    if (audioPath.empty())
        return std::wstring();

    wchar_t buf[MAX_PATH];
    lstrcpynW(buf, audioPath.c_str(), MAX_PATH);
    PathRenameExtensionW(buf, L".mp4");
    return buf;
}

bool BrowseForFile(HWND parent, bool save, const wchar_t* title, const wchar_t* filter,
                   const wchar_t* defaultExt, const std::wstring& startPath,
                   std::wstring& chosen)
{
    wchar_t file[MAX_PATH * 4];
    file[0] = 0;
    if (!startPath.empty())
        lstrcpynW(file, startPath.c_str(), ARRAYSIZE(file));

    const std::wstring dir = DirectoryOf(startPath);

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize     = sizeof(ofn);
    ofn.hwndOwner       = parent;
    ofn.lpstrFilter     = filter;
    ofn.lpstrFile       = file;
    ofn.nMaxFile        = ARRAYSIZE(file);
    ofn.lpstrTitle      = title;
    ofn.lpstrDefExt     = defaultExt;
    ofn.lpstrInitialDir = dir.empty() ? NULL : dir.c_str();

    // OFN_NOCHANGEDIR matters: the visualizer resolves its preset and texture
    // folders against the working directory, so the dialog must not move it.
    ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
    ofn.Flags |= save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST;

    const BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!ok)
        return false;

    chosen = file;
    return true;
}

// Proves the file can actually be created or replaced before a render spends
// twenty minutes discovering it cannot.
bool OutputIsWritable(const std::wstring& path, std::wstring& problem)
{
    const std::wstring dir = DirectoryOf(path);
    if (!dir.empty() && !PathIsDirectoryW(dir.c_str()))
    {
        problem = L"The output folder does not exist: " + dir;
        return false;
    }

    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES)
    {
        if (attrs & FILE_ATTRIBUTE_DIRECTORY)
        {
            problem = L"The output path is a folder. Give the video a file name.";
            return false;
        }

        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE)
        {
            problem = L"That file cannot be replaced. It may be open in another "
                      L"program, or marked read-only.";
            return false;
        }
        CloseHandle(h);
        return true;
    }

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        problem = L"The output file cannot be created there. Check that the folder "
                  L"exists and that you can write to it.";
        return false;
    }
    CloseHandle(h);
    DeleteFileW(path.c_str());
    return true;
}

/////////////////////////////////////////////////////////////////////////////
// Frame arithmetic, matching RunRenderJob exactly

long long EstimateFrameCount(const RenderJobConfig& cfg, double audioSeconds)
{
    if (cfg.fpsNum <= 0 || cfg.fpsDen <= 0 || audioSeconds <= 0.0)
        return 0;

    const double rate = (double)cfg.fpsNum / (double)cfg.fpsDen;

    const double span = (cfg.durationSeconds > 0.0)
                      ? cfg.durationSeconds
                      : (audioSeconds - cfg.startSeconds);
    long long frames = (long long)ceil(span * rate - 1e-9);

    long long total = (long long)ceil(audioSeconds * rate - 1e-9);
    if (total < 1)
        total = 1;

    const long long offset = (cfg.startSeconds > 0.0)
                           ? (long long)(cfg.startSeconds * rate + 0.5)
                           : 0;

    const long long available = total - offset;
    if (frames > available)
        frames = available;

    return (frames > 0) ? frames : 0;
}

/////////////////////////////////////////////////////////////////////////////
// Settings dialog

struct SettingsUi
{
    RenderJobConfig cfg;

    // True once the user has typed their own output path, after which the audio
    // file no longer gets to overwrite it.
    bool outputEdited;

    // Set while the controls are being filled in. Every SetWindowText fires the
    // same EN_CHANGE a keystroke does, and those handlers read the whole dialog
    // back into cfg, so without this the half-populated dialog overwrites the
    // values still waiting to be loaded from it.
    bool loading;

    // Cached duration probe, keyed on the path it was taken from.
    std::wstring probedPath;
    double       probedSeconds;
    std::wstring probeError;

    SettingsUi() : outputEdited(false), loading(false), probedSeconds(0.0) {}
};

// Everything the dialog can check without starting a render. Fills 'focusId' with
// the control that needs attention so the user is not left hunting for it.
bool ValidateConfig(const RenderJobConfig& cfg, double audioSeconds,
                    std::wstring& problem, int& focusId)
{
    focusId = 0;

    if (cfg.presetPath.empty())
    {
        problem = L"Choose the preset to render.";
        focusId = IDC_RD_PRESET;
        return false;
    }
    if (!FileExists(cfg.presetPath))
    {
        problem = L"That preset file does not exist.";
        focusId = IDC_RD_PRESET;
        return false;
    }

    if (cfg.audioPath.empty())
    {
        problem = L"Choose the song to render against. It sets the length of the video.";
        focusId = IDC_RD_AUDIO;
        return false;
    }
    if (!FileExists(cfg.audioPath))
    {
        problem = L"That song file does not exist.";
        focusId = IDC_RD_AUDIO;
        return false;
    }

    if (cfg.outputPath.empty())
    {
        problem = L"Choose where to write the video.";
        focusId = IDC_RD_OUTPUT;
        return false;
    }
    if (!OutputIsWritable(cfg.outputPath, problem))
    {
        focusId = IDC_RD_OUTPUT;
        return false;
    }

    if (cfg.width <= 0 || cfg.height <= 0)
    {
        problem = L"Width and height must both be positive.";
        focusId = IDC_RD_WIDTH;
        return false;
    }
    if ((cfg.width & 1) || (cfg.height & 1))
    {
        problem = L"Width and height must both be even. H.265 subsamples chroma, so "
                  L"an odd dimension cannot be encoded.";
        focusId = (cfg.width & 1) ? IDC_RD_WIDTH : IDC_RD_HEIGHT;
        return false;
    }
    if (cfg.width > kMaxDimension || cfg.height > kMaxDimension)
    {
        problem = Format(L"Width and height must be %d or less.", kMaxDimension);
        focusId = IDC_RD_WIDTH;
        return false;
    }

    if (cfg.fpsNum <= 0 || cfg.fpsDen <= 0)
    {
        problem = L"Frame rate must be a positive number, like 60 or 24000/1001.";
        focusId = IDC_RD_FPS;
        return false;
    }

    if (cfg.pq2020 && cfg.precision != Precision::High)
    {
        problem = L"HDR10 output needs high precision: the conversion works from the "
                  L"float canvas.";
        focusId = IDC_RD_PREC_HIGH;
        return false;
    }

    if (audioSeconds > 0.0 && cfg.startSeconds >= audioSeconds)
    {
        problem = L"The start offset is past the end of the song.";
        focusId = IDC_RD_START;
        return false;
    }

    if (audioSeconds > 0.0 && EstimateFrameCount(cfg, audioSeconds) <= 0)
    {
        problem = L"That start and length leave nothing to render.";
        focusId = IDC_RD_DURATION;
        return false;
    }

    problem.clear();
    return true;
}

// Reads every control into cfg. Bad numbers land as zeros, which validation then
// reports; nothing here silently substitutes a value the user did not ask for.
void ReadControls(HWND hDlg, SettingsUi* ui)
{
    RenderJobConfig& cfg = ui->cfg;

    cfg.presetPath = Trim(GetText(hDlg, IDC_RD_PRESET));
    cfg.audioPath  = Trim(GetText(hDlg, IDC_RD_AUDIO));
    cfg.outputPath = Trim(GetText(hDlg, IDC_RD_OUTPUT));

    cfg.width  = _wtoi(GetText(hDlg, IDC_RD_WIDTH).c_str());
    cfg.height = _wtoi(GetText(hDlg, IDC_RD_HEIGHT).c_str());

    int num = 0, den = 0;
    if (ParseFps(GetText(hDlg, IDC_RD_FPS), num, den))
    {
        cfg.fpsNum = num;
        cfg.fpsDen = den;
    }
    else
    {
        cfg.fpsNum = 0;
        cfg.fpsDen = 0;
    }

    const int pos = (int)SendDlgItemMessageW(hDlg, IDC_RD_QUALITY, TBM_GETPOS, 0, 0);
    cfg.quality = kQualityWorst - pos;

    cfg.precision = IsDlgButtonChecked(hDlg, IDC_RD_PREC_HIGH) == BST_CHECKED
                  ? Precision::High : Precision::Faithful;

    cfg.pq2020 = (cfg.precision == Precision::High) &&
                 IsDlgButtonChecked(hDlg, IDC_RD_PQ2020) == BST_CHECKED;

    const int backend = (int)SendDlgItemMessageW(hDlg, IDC_RD_ENCODER, CB_GETCURSEL, 0, 0);
    switch (backend)
    {
    case 1:  cfg.backend = EncoderBackend::Nvenc;           break;
    case 2:  cfg.backend = EncoderBackend::MediaFoundation; break;
    default: cfg.backend = EncoderBackend::Auto;            break;
    }

    if (IsDlgButtonChecked(hDlg, IDC_RD_SECTION) == BST_CHECKED)
    {
        double start = 0.0, duration = 0.0;
        ParseTime(GetText(hDlg, IDC_RD_START), start);
        ParseTime(GetText(hDlg, IDC_RD_DURATION), duration);
        cfg.startSeconds    = start;
        cfg.durationSeconds = duration;
    }
    else
    {
        cfg.startSeconds    = 0.0;
        cfg.durationSeconds = 0.0;
    }
}

// Opening the file is the only way to know how long it is, so the answer is cached
// and only refreshed when the path changes.
void ProbeAudio(HWND hDlg, SettingsUi* ui)
{
    const std::wstring path = Trim(GetText(hDlg, IDC_RD_AUDIO));
    if (path == ui->probedPath)
        return;

    ui->probedPath    = path;
    ui->probedSeconds = 0.0;
    ui->probeError.clear();

    if (path.empty())
        return;

    if (!FileExists(path))
    {
        ui->probeError = L"That song file does not exist.";
        return;
    }

    HCURSOR previous = SetCursor(LoadCursor(NULL, IDC_WAIT));

    AudioSource audio;
    std::wstring error;
    if (audio.Open(path, error))
        ui->probedSeconds = audio.DurationSeconds();
    else
        ui->probeError = error;

    SetCursor(previous);
}

void SelectSizePreset(HWND hDlg, int width, int height)
{
    int index = ARRAYSIZE(kSizePresets);   // the trailing "Custom" entry
    for (int i = 0; i < ARRAYSIZE(kSizePresets); i++)
    {
        if (kSizePresets[i].width == width && kSizePresets[i].height == height)
        {
            index = i;
            break;
        }
    }
    SendDlgItemMessageW(hDlg, IDC_RD_SIZE, CB_SETCURSEL, index, 0);
}

void UpdateQualityText(HWND hDlg)
{
    const int pos = (int)SendDlgItemMessageW(hDlg, IDC_RD_QUALITY, TBM_GETPOS, 0, 0);
    const int quality = kQualityWorst - pos;

    const wchar_t* word = L"small file";
    if      (quality <= 14) word = L"near lossless";
    else if (quality <= 20) word = L"high";
    else if (quality <= 27) word = L"balanced";

    SetText(hDlg, IDC_RD_QUALITY_TEXT, Format(L"%d, %s", quality, word).c_str());
}

void UpdateEnabledState(HWND hDlg)
{
    const bool high = IsDlgButtonChecked(hDlg, IDC_RD_PREC_HIGH) == BST_CHECKED;
    EnableWindow(GetDlgItem(hDlg, IDC_RD_PQ2020), high ? TRUE : FALSE);
    if (!high)
        CheckDlgButton(hDlg, IDC_RD_PQ2020, BST_UNCHECKED);

    const bool section = IsDlgButtonChecked(hDlg, IDC_RD_SECTION) == BST_CHECKED;
    const int sectionControls[] = {
        IDC_RD_START, IDC_RD_DURATION, IDC_RD_START_LABEL,
        IDC_RD_DURATION_LABEL, IDC_RD_SECTION_HINT
    };
    for (int i = 0; i < ARRAYSIZE(sectionControls); i++)
        EnableWindow(GetDlgItem(hDlg, sectionControls[i]), section ? TRUE : FALSE);
}

// The line that tells the user what they are about to get, kept current as they
// type. A render is a long commitment; the size of it should not be a surprise.
void UpdateSummary(HWND hDlg, SettingsUi* ui)
{
    ReadControls(hDlg, ui);
    const RenderJobConfig& cfg = ui->cfg;

    std::wstring summary;
    if (cfg.width > 0 && cfg.height > 0 && cfg.fpsNum > 0)
        summary = Format(L"%d x %d at %s fps",
                         cfg.width, cfg.height, FpsText(cfg.fpsNum, cfg.fpsDen).c_str());

    if (ui->probedSeconds > 0.0)
    {
        const long long frames = EstimateFrameCount(cfg, ui->probedSeconds);
        if (frames > 0 && !summary.empty())
        {
            const double seconds = frames * (double)cfg.fpsDen / (double)cfg.fpsNum;
            summary += L", " + WithThousands(frames) + L" frames, " + Clock(seconds, true);
            if (cfg.startSeconds > 0.0)
                summary += L" starting at " + Clock(cfg.startSeconds, true);
        }
    }
    else if (cfg.audioPath.empty())
    {
        summary += summary.empty() ? L"" : L".  ";
        summary += L"Choose a song and the video runs for exactly as long as it does.";
    }

    SetText(hDlg, IDC_RD_SUMMARY, summary.c_str());

    // Only complain about fields the user has actually filled in. Everything else
    // waits for the Render button, which reports the first thing still missing.
    double start = 0.0, duration = 0.0;
    const bool sectionTimesOk =
        IsDlgButtonChecked(hDlg, IDC_RD_SECTION) != BST_CHECKED ||
        (ParseTime(GetText(hDlg, IDC_RD_START), start) &&
         ParseTime(GetText(hDlg, IDC_RD_DURATION), duration));

    std::wstring warning = ui->probeError;
    if (warning.empty())
    {
        if (!cfg.presetPath.empty() && !FileExists(cfg.presetPath))
            warning = L"That preset file does not exist.";
        else if (cfg.fpsNum <= 0)
            warning = L"Frame rate must be a number, like 60 or 24000/1001.";
        else if (cfg.width > 0 && cfg.height > 0 && ((cfg.width & 1) || (cfg.height & 1)))
            warning = L"Width and height must both be even.";
        else if (cfg.width > kMaxDimension || cfg.height > kMaxDimension)
            warning = Format(L"Width and height must be %d or less.", kMaxDimension);
        else if (!sectionTimesOk)
            warning = L"Start and length must be seconds or mm:ss.";
    }

    SetText(hDlg, IDC_RD_WARNING, warning.c_str());
}

void LoadControls(HWND hDlg, SettingsUi* ui)
{
    const RenderJobConfig cfg = ui->cfg;   // a copy: ui->cfg is rewritten below
    ui->loading = true;

    for (int i = 0; i < ARRAYSIZE(kSizePresets); i++)
        SendDlgItemMessageW(hDlg, IDC_RD_SIZE, CB_ADDSTRING, 0, (LPARAM)kSizePresets[i].label);
    SendDlgItemMessageW(hDlg, IDC_RD_SIZE, CB_ADDSTRING, 0, (LPARAM)L"Custom");

    for (int i = 0; i < ARRAYSIZE(kFpsPresets); i++)
        SendDlgItemMessageW(hDlg, IDC_RD_FPS, CB_ADDSTRING, 0, (LPARAM)kFpsPresets[i]);

    for (int i = 0; i < ARRAYSIZE(kEncoderLabels); i++)
        SendDlgItemMessageW(hDlg, IDC_RD_ENCODER, CB_ADDSTRING, 0, (LPARAM)kEncoderLabels[i]);

    SendDlgItemMessageW(hDlg, IDC_RD_PRESET, EM_SETLIMITTEXT, MAX_PATH - 1, 0);
    SendDlgItemMessageW(hDlg, IDC_RD_AUDIO,  EM_SETLIMITTEXT, MAX_PATH - 1, 0);
    SendDlgItemMessageW(hDlg, IDC_RD_OUTPUT, EM_SETLIMITTEXT, MAX_PATH - 1, 0);

    SetPathText(hDlg, IDC_RD_PRESET, cfg.presetPath.c_str());
    SetPathText(hDlg, IDC_RD_AUDIO,  cfg.audioPath.c_str());
    SetPathText(hDlg, IDC_RD_OUTPUT, cfg.outputPath.c_str());

    SetDlgItemInt(hDlg, IDC_RD_WIDTH,  cfg.width,  FALSE);
    SetDlgItemInt(hDlg, IDC_RD_HEIGHT, cfg.height, FALSE);
    SelectSizePreset(hDlg, cfg.width, cfg.height);

    SetText(hDlg, IDC_RD_FPS, FpsText(cfg.fpsNum, cfg.fpsDen).c_str());

    SendDlgItemMessageW(hDlg, IDC_RD_QUALITY, TBM_SETRANGE, TRUE,
                        MAKELPARAM(0, kQualityWorst - kQualityBest));
    SendDlgItemMessageW(hDlg, IDC_RD_QUALITY, TBM_SETTICFREQ, 5, 0);
    SendDlgItemMessageW(hDlg, IDC_RD_QUALITY, TBM_SETPAGESIZE, 0, 2);
    SendDlgItemMessageW(hDlg, IDC_RD_QUALITY, TBM_SETPOS, TRUE,
                        kQualityWorst - cfg.quality);
    UpdateQualityText(hDlg);

    CheckRadioButton(hDlg, IDC_RD_PREC_FAITHFUL, IDC_RD_PREC_HIGH,
                     cfg.precision == Precision::High ? IDC_RD_PREC_HIGH
                                                      : IDC_RD_PREC_FAITHFUL);
    CheckDlgButton(hDlg, IDC_RD_PQ2020, cfg.pq2020 ? BST_CHECKED : BST_UNCHECKED);

    int backend = 0;
    if (cfg.backend == EncoderBackend::Nvenc)                backend = 1;
    else if (cfg.backend == EncoderBackend::MediaFoundation) backend = 2;
    SendDlgItemMessageW(hDlg, IDC_RD_ENCODER, CB_SETCURSEL, backend, 0);

    const bool section = (cfg.startSeconds > 0.0 || cfg.durationSeconds > 0.0);
    CheckDlgButton(hDlg, IDC_RD_SECTION, section ? BST_CHECKED : BST_UNCHECKED);
    if (section)
    {
        SetText(hDlg, IDC_RD_START, Clock(cfg.startSeconds, true).c_str());
        if (cfg.durationSeconds > 0.0)
            SetText(hDlg, IDC_RD_DURATION, Clock(cfg.durationSeconds, true).c_str());
    }

    ui->outputEdited = !cfg.outputPath.empty();
    UpdateEnabledState(hDlg);

    ui->loading = false;
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
        LoadControls(hDlg, ui);
        ProbeAudio(hDlg, ui);
        UpdateSummary(hDlg, ui);

        SetFocus(GetDlgItem(hDlg, ui->cfg.audioPath.empty() ? IDC_RD_AUDIO : IDOK));
        return FALSE;   // focus was set here
    }

    case WM_TIMER:
        if (wp == kProbeTimer)
        {
            KillTimer(hDlg, kProbeTimer);
            ProbeAudio(hDlg, ui);
            UpdateSummary(hDlg, ui);
        }
        return TRUE;

    case WM_CTLCOLORSTATIC:
        if (GetDlgCtrlID((HWND)lp) == IDC_RD_WARNING)
        {
            SetTextColor((HDC)wp, RGB(176, 32, 32));
            SetBkMode((HDC)wp, TRANSPARENT);
            return (INT_PTR)GetSysColorBrush(COLOR_BTNFACE);
        }
        return FALSE;

    case WM_HSCROLL:
        if ((HWND)lp == GetDlgItem(hDlg, IDC_RD_QUALITY))
        {
            UpdateQualityText(hDlg);
            return TRUE;
        }
        return FALSE;

    case WM_COMMAND:
    {
        if (!ui || ui->loading)
            return TRUE;

        const int id   = LOWORD(wp);
        const int code = HIWORD(wp);

        switch (id)
        {
        case IDC_RD_PRESET_BROWSE:
        {
            std::wstring chosen;
            if (BrowseForFile(hDlg, false, L"Choose a preset",
                              L"MilkDrop presets (*.milk)\0*.milk\0All files (*.*)\0*.*\0",
                              L"milk", Trim(GetText(hDlg, IDC_RD_PRESET)), chosen))
            {
                SetPathText(hDlg, IDC_RD_PRESET, chosen.c_str());
                UpdateSummary(hDlg, ui);
            }
            return TRUE;
        }

        case IDC_RD_AUDIO_BROWSE:
        {
            std::wstring chosen;
            if (BrowseForFile(hDlg, false, L"Choose a song",
                              L"Audio files\0*.wav;*.flac;*.mp3;*.m4a;*.aac;*.wma;*.aif;*.aiff\0"
                              L"All files (*.*)\0*.*\0",
                              NULL, Trim(GetText(hDlg, IDC_RD_AUDIO)), chosen))
            {
                SetPathText(hDlg, IDC_RD_AUDIO, chosen.c_str());
                KillTimer(hDlg, kProbeTimer);
                ProbeAudio(hDlg, ui);
                UpdateSummary(hDlg, ui);
            }
            return TRUE;
        }

        case IDC_RD_OUTPUT_BROWSE:
        {
            std::wstring start = Trim(GetText(hDlg, IDC_RD_OUTPUT));
            if (start.empty())
                start = DefaultOutputFor(Trim(GetText(hDlg, IDC_RD_AUDIO)));

            std::wstring chosen;
            if (BrowseForFile(hDlg, true, L"Write the video to",
                              L"MP4 video (*.mp4)\0*.mp4\0All files (*.*)\0*.*\0",
                              L"mp4", start, chosen))
            {
                ui->outputEdited = true;
                SetPathText(hDlg, IDC_RD_OUTPUT, chosen.c_str());
                UpdateSummary(hDlg, ui);
            }
            return TRUE;
        }

        case IDC_RD_AUDIO:
            if (code == EN_CHANGE)
            {
                if (!ui->outputEdited)
                    SetPathText(hDlg, IDC_RD_OUTPUT,
                                DefaultOutputFor(Trim(GetText(hDlg, IDC_RD_AUDIO))).c_str());

                // Opening the file costs real time, so wait for the typing to stop.
                SetTimer(hDlg, kProbeTimer, kProbeDelayMs, NULL);
                UpdateSummary(hDlg, ui);
            }
            else if (code == EN_KILLFOCUS)
            {
                KillTimer(hDlg, kProbeTimer);
                ProbeAudio(hDlg, ui);
                UpdateSummary(hDlg, ui);
            }
            return TRUE;

        case IDC_RD_OUTPUT:
            if (code == EN_CHANGE && GetFocus() == GetDlgItem(hDlg, IDC_RD_OUTPUT))
                ui->outputEdited = true;
            return TRUE;

        case IDC_RD_SIZE:
            if (code == CBN_SELCHANGE)
            {
                const int sel = (int)SendDlgItemMessageW(hDlg, IDC_RD_SIZE, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < ARRAYSIZE(kSizePresets))
                {
                    SetDlgItemInt(hDlg, IDC_RD_WIDTH,  kSizePresets[sel].width,  FALSE);
                    SetDlgItemInt(hDlg, IDC_RD_HEIGHT, kSizePresets[sel].height, FALSE);
                }
                UpdateSummary(hDlg, ui);
            }
            return TRUE;

        case IDC_RD_WIDTH:
        case IDC_RD_HEIGHT:
            if (code == EN_CHANGE)
            {
                // Typing a size that is not one of the presets is not an error, it
                // just means the combo should stop claiming otherwise.
                SelectSizePreset(hDlg,
                                 _wtoi(GetText(hDlg, IDC_RD_WIDTH).c_str()),
                                 _wtoi(GetText(hDlg, IDC_RD_HEIGHT).c_str()));
                UpdateSummary(hDlg, ui);
            }
            return TRUE;

        case IDC_RD_FPS:
            if (code == CBN_SELCHANGE)
            {
                // A drop-down combo has not copied the selection into its edit
                // field yet when this arrives, and the summary reads that field.
                const int sel = (int)SendDlgItemMessageW(hDlg, IDC_RD_FPS, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < ARRAYSIZE(kFpsPresets))
                    SetText(hDlg, IDC_RD_FPS, kFpsPresets[sel]);
                UpdateSummary(hDlg, ui);
            }
            else if (code == CBN_EDITCHANGE)
            {
                UpdateSummary(hDlg, ui);
            }
            return TRUE;

        case IDC_RD_PREC_FAITHFUL:
        case IDC_RD_PREC_HIGH:
        case IDC_RD_SECTION:
            UpdateEnabledState(hDlg);
            UpdateSummary(hDlg, ui);
            return TRUE;

        case IDC_RD_START:
        case IDC_RD_DURATION:
            if (code == EN_CHANGE)
                UpdateSummary(hDlg, ui);
            return TRUE;

        case IDC_RD_PRESET:
            if (code == EN_CHANGE)
                UpdateSummary(hDlg, ui);
            return TRUE;

        case IDOK:
        {
            KillTimer(hDlg, kProbeTimer);
            ProbeAudio(hDlg, ui);
            ReadControls(hDlg, ui);

            // A song the decoder has already refused is worth reporting here rather
            // than a minute into a render that was never going to work.
            std::wstring problem = ui->probeError;
            int focusId = problem.empty() ? 0 : IDC_RD_AUDIO;

            if (!problem.empty() || !ValidateConfig(ui->cfg, ui->probedSeconds, problem, focusId))
            {
                SetText(hDlg, IDC_RD_WARNING, problem.c_str());
                MessageBoxW(hDlg, problem.c_str(), L"Cannot render yet",
                            MB_OK | MB_ICONWARNING);
                if (focusId)
                    SetFocus(GetDlgItem(hDlg, focusId));
                return TRUE;
            }

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
        KillTimer(hDlg, kProbeTimer);
        return FALSE;
    }

    return FALSE;
}

/////////////////////////////////////////////////////////////////////////////
// Progress dialog

struct ProgressUi
{
    HINSTANCE       hInstance;
    RenderJobConfig cfg;
    HWND            hDlg;
    HANDLE          thread;

    CRITICAL_SECTION lock;
    long long        frameIndex;
    long long        frameCount;
    double           elapsed;

    volatile LONG    cancelRequested;

    RenderResult     result;
    bool             playWhenDone;

    // Windowed throughput, so the number reflects what the render is doing now
    // rather than an average dragged down by a slow start.
    long long        sampleFrame;
    double           sampleTime;
    double           smoothedFps;
    bool             determinate;

    ProgressUi()
        : hInstance(NULL), hDlg(NULL), thread(NULL),
          frameIndex(0), frameCount(0), elapsed(0.0), cancelRequested(0),
          playWhenDone(true), sampleFrame(0), sampleTime(0.0),
          smoothedFps(0.0), determinate(false)
    {
        InitializeCriticalSection(&lock);
    }

    ~ProgressUi()
    {
        DeleteCriticalSection(&lock);
    }
};

bool OnRenderProgress(void* ctx, const RenderProgress& p)
{
    ProgressUi* ui = (ProgressUi*)ctx;

    EnterCriticalSection(&ui->lock);
    ui->frameIndex = p.frameIndex;
    ui->frameCount = p.frameCount;
    ui->elapsed    = p.elapsedSeconds;
    LeaveCriticalSection(&ui->lock);

    return InterlockedCompareExchange(&ui->cancelRequested, 0, 0) == 0;
}

unsigned __stdcall RenderThread(void* param)
{
    ProgressUi* ui = (ProgressUi*)param;

    // Deliberately no CoInitializeEx here: the muxer starts and stops Media
    // Foundation itself, and this is the same apartment state the command-line
    // path renders in.
    ui->result = RunRenderJob(ui->hInstance, ui->cfg, OnRenderProgress, ui);

    PostMessageW(ui->hDlg, WM_RENDER_FINISHED, 0, 0);
    return 0;
}

void SetMarquee(HWND bar, bool on)
{
    LONG_PTR style = GetWindowLongPtrW(bar, GWL_STYLE);
    if (on)
    {
        SetWindowLongPtrW(bar, GWL_STYLE, style | PBS_MARQUEE);
        SendMessageW(bar, PBM_SETMARQUEE, TRUE, 30);
    }
    else
    {
        SendMessageW(bar, PBM_SETMARQUEE, FALSE, 0);
        SetWindowLongPtrW(bar, GWL_STYLE, style & ~PBS_MARQUEE);
    }
}

void TickProgress(HWND hDlg, ProgressUi* ui)
{
    long long index, count;
    double elapsed;

    EnterCriticalSection(&ui->lock);
    index   = ui->frameIndex;
    count   = ui->frameCount;
    elapsed = ui->elapsed;
    LeaveCriticalSection(&ui->lock);

    if (count <= 0)
        return;   // still starting up; the marquee says so

    HWND bar = GetDlgItem(hDlg, IDC_RP_BAR);
    if (!ui->determinate)
    {
        ui->determinate = true;
        SetMarquee(bar, false);
        SendMessageW(bar, PBM_SETRANGE32, 0, (LPARAM)count);

        SetText(hDlg, IDC_RP_HEADING,
                Format(L"Rendering %d x %d at %s fps",
                       ui->cfg.width, ui->cfg.height,
                       FpsText(ui->cfg.fpsNum, ui->cfg.fpsDen).c_str()).c_str());
    }
    SendMessageW(bar, PBM_SETPOS, (WPARAM)index, 0);

    const int percent = (int)(index * 100 / count);
    SetText(hDlg, IDC_RP_FRAMES,
            Format(L"%s of %s frames (%d%%)",
                   WithThousands(index).c_str(), WithThousands(count).c_str(),
                   percent).c_str());

    const double window = elapsed - ui->sampleTime;
    if (window >= 0.5)
    {
        const double instant = (index - ui->sampleFrame) / window;
        ui->smoothedFps = (ui->smoothedFps > 0.0)
                        ? (ui->smoothedFps * 0.7 + instant * 0.3)
                        : instant;
        ui->sampleFrame = index;
        ui->sampleTime  = elapsed;
    }

    const double average = (elapsed > 0.0) ? (index / elapsed) : 0.0;
    SetText(hDlg, IDC_RP_RATE,
            Format(L"%.1f frames per second", ui->smoothedFps > 0.0 ? ui->smoothedFps : average).c_str());

    SetText(hDlg, IDC_RP_ELAPSED, (L"Elapsed " + Clock(elapsed)).c_str());

    // An estimate off the first second of a render is worse than no estimate, so
    // it stays hidden until there is enough of one to mean something.
    if (index >= 20 && average > 0.0)
        SetText(hDlg, IDC_RP_REMAIN,
                (Clock((count - index) / average) + L" to go").c_str());
}

INT_PTR CALLBACK ProgressProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    ProgressUi* ui = (ProgressUi*)GetWindowLongPtrW(hDlg, DWLP_USER);

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        ui = (ProgressUi*)lp;
        SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)ui);
        ui->hDlg = hDlg;

        InheritOwnerIcon(hDlg);

        SetText(hDlg, IDC_RP_HEADING, L"Starting the render");
        SetText(hDlg, IDC_RP_FILE, ui->cfg.outputPath.c_str());
        SetText(hDlg, IDC_RP_ELAPSED, L"Elapsed 0:00");
        SetText(hDlg, IDC_RP_REMAIN, L"Estimating");
        CheckDlgButton(hDlg, IDC_RP_PLAY, ui->playWhenDone ? BST_CHECKED : BST_UNCHECKED);
        SetMarquee(GetDlgItem(hDlg, IDC_RP_BAR), true);

        ui->thread = (HANDLE)_beginthreadex(NULL, 0, RenderThread, ui, 0, NULL);
        if (!ui->thread)
        {
            ui->result.ok    = false;
            ui->result.error = L"Could not start the render thread.";
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }

        SetTimer(hDlg, kTickTimer, kTickMs, NULL);
        return TRUE;
    }

    case WM_TIMER:
        if (wp == kTickTimer)
            TickProgress(hDlg, ui);
        return TRUE;

    case WM_RENDER_FINISHED:
        KillTimer(hDlg, kTickTimer);
        ui->playWhenDone = IsDlgButtonChecked(hDlg, IDC_RP_PLAY) == BST_CHECKED;
        EndDialog(hDlg, ui->result.ok ? IDOK : IDCANCEL);
        return TRUE;

    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL)
        {
            // The render holds the D3D device and the encoder session, so the
            // dialog cannot go away until the worker has unwound them. Ask, then
            // wait: RunRenderJob checks the flag once per frame.
            if (InterlockedExchange(&ui->cancelRequested, 1) == 0)
            {
                SetText(hDlg, IDCANCEL, L"Stopping");
                EnableWindow(GetDlgItem(hDlg, IDCANCEL), FALSE);
                SetText(hDlg, IDC_RP_HEADING, L"Stopping the render");
            }
            return TRUE;
        }
        return FALSE;

    case WM_CLOSE:
        // Alt+F4 and the close box mean the same thing as Cancel here.
        SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
        return TRUE;
    }

    return FALSE;
}

/////////////////////////////////////////////////////////////////////////////
// What the user last asked for, so a second render does not start from scratch

RenderJobConfig g_lastConfig;
bool            g_playWhenDone = true;

} // namespace

const RenderJobConfig& LastRenderConfig()
{
    return g_lastConfig;
}

bool ShowRenderSettingsDialog(HINSTANCE hInstance, HWND parent,
                              const wchar_t* currentPresetPath,
                              RenderJobConfig& cfg)
{
    ComScope   com;
    ThemeScope theme(hInstance);
    DpiScope   dpi;

    SettingsUi ui;
    ui.cfg = cfg;

    if (ui.cfg.presetPath.empty() && currentPresetPath && *currentPresetPath)
        ui.cfg.presetPath = currentPresetPath;

    if (ui.cfg.outputPath.empty())
        ui.cfg.outputPath = DefaultOutputFor(ui.cfg.audioPath);

    const INT_PTR r = DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_RENDER_SETTINGS),
                                      parent, SettingsProc, (LPARAM)&ui);
    if (r == -1)
    {
        MessageBoxW(parent,
                    L"The render settings dialog could not be created. Its resources "
                    L"are missing from this build.",
                    L"Cannot render", MB_OK | MB_ICONERROR);
        return false;
    }
    if (r != IDOK)
        return false;

    cfg = ui.cfg;
    g_lastConfig = cfg;
    return true;
}

bool RunRenderWithProgress(HINSTANCE hInstance, HWND parent, const RenderJobConfig& cfg,
                           bool* cancelled)
{
    if (cancelled)
        *cancelled = false;

    ComScope   com;
    ThemeScope theme(hInstance);
    DpiScope   dpi;

    ProgressUi ui;
    ui.hInstance    = hInstance;
    ui.cfg          = cfg;
    ui.playWhenDone = g_playWhenDone;

    const INT_PTR r = DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_RENDER_PROGRESS),
                                      parent, ProgressProc, (LPARAM)&ui);
    if (r == -1 && ui.result.error.empty())
        ui.result.error = L"The render progress dialog could not be created. Its "
                          L"resources are missing from this build.";

    if (ui.thread)
    {
        // The dialog only ends on WM_RENDER_FINISHED, so this returns at once. It
        // exists so the thread handle is never closed out from under a live thread.
        WaitForSingleObject(ui.thread, INFINITE);
        CloseHandle(ui.thread);
        ui.thread = NULL;
    }

    g_playWhenDone = ui.playWhenDone;
    if (cancelled)
        *cancelled = (ui.cancelRequested != 0);

    if (ui.result.ok)
    {
        if (ui.playWhenDone)
        {
            ShellExecuteW(NULL, L"open", cfg.outputPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
        }
        else
        {
            const double rate = (ui.result.elapsedSeconds > 0.0)
                              ? (ui.result.framesWritten / ui.result.elapsedSeconds)
                              : 0.0;
            const std::wstring text =
                Format(L"Wrote %s frames in %s (%.1f frames per second).\n\n%s",
                       WithThousands(ui.result.framesWritten).c_str(),
                       Clock(ui.result.elapsedSeconds).c_str(),
                       rate, cfg.outputPath.c_str());
            MessageBoxW(parent, text.c_str(), L"Render finished", MB_OK | MB_ICONINFORMATION);
        }
        return true;
    }

    // A cancel is a decision, not a failure, and does not need reporting back.
    if (!ui.cancelRequested && !ui.result.error.empty())
        MessageBoxW(parent, ui.result.error.c_str(), L"Render failed", MB_OK | MB_ICONERROR);

    return false;
}

} // namespace offline
