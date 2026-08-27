#include "first_run.h"
#include "first_run_ids.h"

// Only for IDR_RENDER_DIALOG_MANIFEST. One activation context resource serves the
// whole app; a second copy of the same manifest would be dead weight in the binary.
#include "render_dialog_ids.h"

#include "../plugin.h"
#include "../utility.h"   // the GetPrivateProfile*/WritePrivateProfile* wrappers

#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <process.h>
#include <stdio.h>
#include <algorithm>
#include <deque>
#include <set>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

extern CPlugin g_plugin;

namespace app {

namespace {

// Posted by the scan thread once it has run out of roots, time or budget. The
// dialog drains results on a timer until this arrives; without it there is no way
// to tell "found nothing yet" from "found nothing at all", and those two need
// different words on screen.
const UINT WM_FIRSTRUN_SCAN_DONE = WM_APP + 1;

const UINT_PTR kDrainTimer = 1;
const int      kDrainMs    = 150;

// The scan runs while the user is reading the window, so it gets a hard ceiling
// rather than a promise to be quick. Whatever it has by then is what it shows, and
// Browse covers the rest. Both limits are deliberately generous for a warm cache
// and deliberately fatal for a cold one on a spinning disk.
const ULONGLONG kScanBudgetMs   = 4000;
const int       kScanBudgetDirs = 25000;

// Mirrors texture_exts in plugin.cpp, which is what the preset loader will
// actually try. Counting anything else would report textures the app cannot use.
const wchar_t* const kTextureExts[] = { L"jpg", L"dds", L"png", L"tga", L"bmp", L"dib" };

// Set on the way out when the user ticks the box, and read by FirstRunNeeded.
// Nothing else in the app touches it.
//
// Not const, because utility.h declares WritePrivateProfileIntW's key and section
// as wchar_t* rather than const wchar_t*. Every key name in settings_dialog.cpp
// reaches it as a bare literal for the same reason.
wchar_t kSkipKey[] = L"bSkipFirstRunSetup";

/////////////////////////////////////////////////////////////////////////////
// Process environment for the dialog
//
// Same three scopes the render and settings dialogs use, and for the same reasons.
// They are deliberately duplicated rather than shared: each dialog keeps them in
// its own anonymous namespace, which is where they belong.

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

// Version 6 common controls, for the list view. The app embeds no application
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

        // Has to happen inside the context, otherwise the list view class comes
        // from the version 5 library: no explorer theme, no hot tracking, and a
        // header that does not match the rest of the app.
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof(icc);
        icc.dwICC  = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
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

std::wstring Format(const wchar_t* fmt, ...)
{
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, fmt, args);
    va_end(args);
    return buf;
}

void SetText(HWND hDlg, int id, const wchar_t* text)
{
    SetDlgItemTextW(hDlg, id, text);
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

/////////////////////////////////////////////////////////////////////////////
// Paths

bool DirectoryExists(const std::wstring& path)
{
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// The whole visualizer builds preset paths by concatenation, so a directory here
// without its trailing backslash produces filenames one character short of real.
std::wstring WithTrailingSlash(const std::wstring& path)
{
    if (path.empty() || path[path.size() - 1] == L'\\')
        return path;
    return path + L"\\";
}

// Everything inside the scan holds paths without the trailing slash, so joining is
// one rule rather than two and the dedup key is unambiguous.
std::wstring NoTrailingSlash(const std::wstring& path)
{
    std::wstring s = path;
    // A bare drive root is "C:\" and stays that way; stripping it leaves "C:",
    // which the file APIs read as the drive's current directory instead.
    while (s.size() > 3 && s[s.size() - 1] == L'\\')
        s.erase(s.size() - 1);
    return s;
}

std::wstring Join(const std::wstring& dir, const wchar_t* leaf)
{
    const std::wstring d = NoTrailingSlash(dir);
    if (!d.empty() && d[d.size() - 1] == L'\\')
        return d + leaf;          // drive root, already ends in a slash
    return d + L"\\" + leaf;
}

std::wstring ParentOf(const std::wstring& path)
{
    const std::wstring p = NoTrailingSlash(path);
    const size_t cut = p.find_last_of(L'\\');
    if (cut == std::wstring::npos || cut < 2)
        return std::wstring();
    return p.substr(0, cut);
}

std::wstring LeafOf(const std::wstring& path)
{
    const std::wstring p = NoTrailingSlash(path);
    const size_t cut = p.find_last_of(L'\\');
    return (cut == std::wstring::npos) ? p : p.substr(cut + 1);
}

std::wstring LowerCase(const std::wstring& s)
{
    std::wstring out = s;
    for (size_t i = 0; i < out.size(); i++)
        out[i] = (wchar_t)towlower(out[i]);
    return out;
}

// Steam writes its registry paths with forward slashes, and a vdf carries doubled
// backslashes. Both are valid to open and neither compares equal to anything else
// we hold, which is what matters for the dedup set.
std::wstring NormalizeSlashes(const std::wstring& path)
{
    std::wstring out = path;
    for (size_t i = 0; i < out.size(); i++)
        if (out[i] == L'/')
            out[i] = L'\\';
    return out;
}

// The list has room for a folder name, not for a path. Two candidates can end in
// the same "Milkdrop3\presets", so keeping three components apart is the least
// that still tells them apart at a glance; the full path sits under the list.
std::wstring ShortPath(const std::wstring& path, int components)
{
    size_t cut = path.size();
    int seen = 0;
    for (size_t i = path.size(); i-- > 0; )
    {
        if (path[i] == L'\\' && ++seen == components)
        {
            cut = i + 1;
            break;
        }
    }
    if (cut == 0 || cut >= path.size())
        return path;
    return std::wstring(L"...\\") + path.substr(cut);
}

/////////////////////////////////////////////////////////////////////////////
// Counting

// Enumerate everything and test the extension, rather than asking for "*.milk".
// A wildcard extension also matches 8.3 short names, so the mask can count a file
// __UpdatePresetList would skip. That one tests the last five characters, so this
// does too.
//
// The visualizer additionally drops presets whose pixel shader version it cannot
// run, which it can only tell by opening each file. This count is therefore an
// upper bound on what ends up in the preset list, and reading 475 files to close
// that gap is not worth the wait.
int CountMilkFiles(const std::wstring& dir)
{
    WIN32_FIND_DATAW ffd;
    HANDLE h = FindFirstFileW(Join(dir, L"*").c_str(), &ffd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;

    int n = 0;
    do
    {
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        const int len = lstrlenW(ffd.cFileName);
        if (len >= 5 && _wcsicmp(ffd.cFileName + len - 5, L".milk") == 0)
            n++;
    } while (FindNextFileW(h, &ffd));

    FindClose(h);
    return n;
}

int CountTextureFiles(const std::wstring& dir)
{
    WIN32_FIND_DATAW ffd;
    HANDLE h = FindFirstFileW(Join(dir, L"*").c_str(), &ffd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;

    int n = 0;
    do
    {
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        const wchar_t* dot = wcsrchr(ffd.cFileName, L'.');
        if (!dot)
            continue;

        for (int i = 0; i < ARRAYSIZE(kTextureExts); i++)
        {
            if (_wcsicmp(dot + 1, kTextureExts[i]) == 0)
            {
                n++;
                break;
            }
        }
    } while (FindNextFileW(h, &ffd));

    FindClose(h);
    return n;
}

// What a directory really is, regardless of the name it was reached by. The volume
// GUID form is the one that answers that: for a mounted volume, the DOS form comes
// back as the mount point itself, so two names for one directory still look like
// two directories.
//
// Empty when the directory cannot be opened, which is the answer for a cloud
// placeholder, an app execution alias, and a permission the user does not have.
std::wstring RealIdentity(const std::wstring& dir)
{
    HANDLE h = CreateFileW(dir.c_str(), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return std::wstring();

    wchar_t buf[1024];
    const DWORD n = GetFinalPathNameByHandleW(h, buf, ARRAYSIZE(buf),
                                              FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
    CloseHandle(h);

    if (n == 0 || n >= ARRAYSIZE(buf))
        return std::wstring();

    return LowerCase(NoTrailingSlash(buf));
}

// Presets reference textures by bare name, and the loader resolves them against
// one configured directory. A pack ships its textures as a sibling of its presets
// folder, so that is the only place worth looking; picking the presets without it
// is what leaves a preset drawing flat grey where an image should be.
std::wstring SiblingTextureDir(const std::wstring& presetDir)
{
    const std::wstring parent = ParentOf(presetDir);
    if (parent.empty())
        return std::wstring();

    const std::wstring candidate = Join(parent, L"textures");
    return DirectoryExists(candidate) ? candidate : std::wstring();
}

/////////////////////////////////////////////////////////////////////////////
// What the scan produces

struct Found
{
    std::wstring   path;       // no trailing backslash
    const wchar_t* where;      // a literal, so copying this across threads is free
    int            presets;
    int            textures;

    Found() : where(L""), presets(0), textures(0) {}
};

// Most presets first, because that is what "best" means to someone who has none.
// Textures break the tie: two copies of the same pack are common, and the one
// that kept its textures is the one worth having.
bool BetterFirst(const Found& a, const Found& b)
{
    if (a.presets  != b.presets)  return a.presets  > b.presets;
    if (a.textures != b.textures) return a.textures > b.textures;
    return _wcsicmp(a.path.c_str(), b.path.c_str()) < 0;
}

struct ScanState
{
    CRITICAL_SECTION   lock;
    std::vector<Found> found;      // append only, guarded
    HWND               hDlg;
    volatile LONG      cancel;

    // The sweep ran out of time or budget with roots left. What is on screen is
    // then a partial answer, and saying so is the difference between "there is
    // nothing else" and "there may well be, press Browse".
    volatile LONG      cutShort;

    ScanState() : hDlg(NULL), cancel(0), cutShort(0)
    {
        InitializeCriticalSection(&lock);
    }
    ~ScanState()
    {
        DeleteCriticalSection(&lock);
    }

private:
    ScanState(const ScanState&);
    ScanState& operator=(const ScanState&);
};

/////////////////////////////////////////////////////////////////////////////
// The sweep
//
// Breadth first, one root at a time, with the roots ordered by how likely they are
// to hold presets. Breadth first is the point: a depth-first walk that runs out of
// budget has spent all of it inside whichever deep tree it entered first, while
// this one has always finished the shallow levels, which is where installs live.

struct SweepRoot
{
    std::wstring   path;
    int            maxDepth;
    const wchar_t* where;

    SweepRoot() : maxDepth(0), where(L"") {}
    SweepRoot(const std::wstring& p, int d, const wchar_t* w) : path(p), maxDepth(d), where(w) {}
};

// The reparse flag comes from the parent's directory listing, which already had
// it. Asking the file system again for every one of thousands of directories, to
// learn that almost none of them is a junction, is the cost this avoids.
struct QueueItem
{
    std::wstring path;
    int          depth;
    bool         reparse;

    QueueItem(const std::wstring& p, int d, bool r) : path(p), depth(d), reparse(r) {}
};

// Directories that cannot hold a preset pack and can hold a hundred thousand
// entries. Skipping them is the difference between the profile sweep finishing and
// the profile sweep being the reason the budget ran out.
bool IsSkippedDirName(const wchar_t* name)
{
    static const wchar_t* const kSkip[] = {
        L"AppData", L"Application Data", L"Local Settings", L"node_modules",
        L"$Recycle.Bin", L"System Volume Information", L"Windows", L"WinSxS",
        L"__pycache__", L"site-packages", L"Temp", L"Temporary Internet Files",
        L"Recent", L"SendTo", L"NetHood", L"PrintHood", L"Cookies",
        L"Start Menu", L"Templates", L"obj",
    };

    // Tool caches, every one of them: .cargo, .rustup, .nuget, .vscode. None has
    // ever held a preset and several are enormous.
    if (name[0] == L'.')
        return true;

    for (int i = 0; i < ARRAYSIZE(kSkip); i++)
        if (_wcsicmp(name, kSkip[i]) == 0)
            return true;

    return false;
}

class Sweeper
{
public:
    Sweeper(ScanState& state)
        : m_state(state),
          m_deadline(GetTickCount64() + kScanBudgetMs),
          m_budget(kScanBudgetDirs)
    {
    }

    // False once the scan is out of time, out of budget, or the dialog has gone
    // away. Every loop in here checks it, so the worker unwinds promptly.
    bool KeepGoing() const
    {
        if (InterlockedCompareExchange(&m_state.cancel, 0, 0) != 0)
            return false;
        if (m_budget <= 0)
            return false;
        return GetTickCount64() < m_deadline;
    }

    void AddRoot(const std::wstring& path, int maxDepth, const wchar_t* where)
    {
        if (path.empty())
            return;

        const std::wstring clean = NoTrailingSlash(NormalizeSlashes(path));
        if (!DirectoryExists(clean))
            return;

        // Four registry keys and two vdf files name the same Steam install, and
        // the same folder swept twice costs the budget twice.
        if (!m_rootsAdded.insert(LowerCase(clean)).second)
            return;

        m_roots.push_back(SweepRoot(clean, maxDepth, where));
    }

    void Run()
    {
        // Index rather than an iterator: a steamapps directory met mid-sweep
        // appends a root, and that has to be swept too.
        for (size_t i = 0; i < m_roots.size(); i++)
        {
            if (!KeepGoing())
            {
                InterlockedExchange(&m_state.cutShort, 1);
                return;
            }
            SweepOne(m_roots[i]);
        }
    }

private:
    void SweepOne(const SweepRoot& root)
    {
        std::deque<QueueItem> queue;
        queue.push_back(QueueItem(root.path, 0, IsReparsePoint(root.path)));

        while (!queue.empty())
        {
            if (!KeepGoing())
            {
                InterlockedExchange(&m_state.cutShort, 1);
                return;
            }

            const QueueItem item = queue.front();
            queue.pop_front();

            if (!Claim(item))
                continue;

            m_budget--;
            Visit(item.path, item.depth, root, queue);
        }
    }

    static bool IsReparsePoint(const std::wstring& dir)
    {
        const DWORD attrs = GetFileAttributesW(dir.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    }

    // One claim per directory the sweep is about to walk, false when somebody
    // already walked it.
    //
    // Refusing every junction outright is the obvious cycle guard and it is wrong
    // here: a second disk mounted into the profile is a junction, and that is
    // exactly where a preset pack ends up on a machine whose C: drive is full.
    // Junctions get claimed by what they really point at instead, so a loop is
    // refused on its second lap while a mounted volume is swept normally.
    bool Claim(const QueueItem& item)
    {
        if (!m_visited.insert(LowerCase(item.path)).second)
            return false;

        if (!item.reparse)
            return true;

        const std::wstring identity = RealIdentity(item.path);
        if (identity.empty())
            return false;

        return m_visited.insert(identity).second;
    }

    // One FindFirstFile pass per directory does both jobs: it lists the children to
    // queue and counts this directory's own presets. Asking twice would double the
    // cost of the whole sweep for nothing.
    void Visit(const std::wstring& dir, int depth, const SweepRoot& root,
               std::deque<QueueItem>& queue)
    {
        WIN32_FIND_DATAW ffd;
        HANDLE h = FindFirstFileW(Join(dir, L"*").c_str(), &ffd);
        if (h == INVALID_HANDLE_VALUE)
            return;

        int milk = 0;

        do
        {
            if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                const int len = lstrlenW(ffd.cFileName);
                if (len >= 5 && _wcsicmp(ffd.cFileName + len - 5, L".milk") == 0)
                    milk++;
                continue;
            }

            if (ffd.cFileName[0] == L'.' &&
                (ffd.cFileName[1] == L'\0' || (ffd.cFileName[1] == L'.' && ffd.cFileName[2] == L'\0')))
                continue;

            if (IsSkippedDirName(ffd.cFileName))
                continue;

            const std::wstring child = Join(dir, ffd.cFileName);

            // A Steam library can live anywhere the user dragged it, and only the
            // ones Steam itself recorded are in libraryfolders.vdf. Meeting the
            // directory is the reliable way to find the rest.
            if (_wcsicmp(ffd.cFileName, L"steamapps") == 0)
                AddRoot(Join(child, L"common"), 2, L"Steam");

            if (depth < root.maxDepth)
            {
                queue.push_back(QueueItem(child, depth + 1,
                                          (ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0));
            }

        } while (FindNextFileW(h, &ffd));

        FindClose(h);

        if (milk > 0)
            Record(dir, milk, root.where);
    }

    void Record(const std::wstring& dir, int presets, const wchar_t* where)
    {
        // One row per real folder. A pack under a mounted volume is reachable
        // both through the mount point and through the volume's own letter, and
        // two rows for one folder is a choice the user cannot make.
        const std::wstring identity = RealIdentity(dir);
        if (!identity.empty() && !m_recorded.insert(identity).second)
            return;

        Found f;
        f.path     = dir;
        f.where    = where;
        f.presets  = presets;

        const std::wstring textures = SiblingTextureDir(dir);
        f.textures = textures.empty() ? 0 : CountTextureFiles(textures);

        EnterCriticalSection(&m_state.lock);
        m_state.found.push_back(f);
        LeaveCriticalSection(&m_state.lock);
    }

    ScanState&              m_state;
    std::vector<SweepRoot>  m_roots;
    std::set<std::wstring>  m_rootsAdded;
    std::set<std::wstring>  m_visited;
    std::set<std::wstring>  m_recorded;
    ULONGLONG               m_deadline;
    int                     m_budget;

    Sweeper(const Sweeper&);
    Sweeper& operator=(const Sweeper&);
};

/////////////////////////////////////////////////////////////////////////////
// Where to look

std::wstring ReadRegString(HKEY root, const wchar_t* subkey, const wchar_t* value, REGSAM extra)
{
    HKEY key = NULL;
    if (RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE | extra, &key) != ERROR_SUCCESS)
        return std::wstring();

    wchar_t buf[MAX_PATH];
    DWORD cb   = sizeof(buf);
    DWORD type = 0;
    const LONG r = RegQueryValueExW(key, value, NULL, &type, (LPBYTE)buf, &cb);
    RegCloseKey(key);

    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
        return std::wstring();

    // RegQueryValueEx does not promise a terminator, and cb is bytes.
    size_t chars = cb / sizeof(wchar_t);
    if (chars >= ARRAYSIZE(buf))
        chars = ARRAYSIZE(buf) - 1;
    buf[chars] = L'\0';

    return buf;
}

std::wstring Env(const wchar_t* name)
{
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(name, buf, ARRAYSIZE(buf));
    if (n == 0 || n >= ARRAYSIZE(buf))
        return std::wstring();
    return buf;
}

// libraryfolders.vdf is Valve's own key/value text: quoted tokens in pairs, with
// backslashes doubled. Every "path" entry names a library root. Reading it is the
// only way to find a library on another drive.
void ParseLibraryFoldersVdf(const std::wstring& file, std::vector<std::wstring>& out)
{
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;

    const DWORD size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size == 0 || size > (1u << 20))
    {
        CloseHandle(h);
        return;
    }

    std::string bytes((size_t)size, '\0');
    DWORD got = 0;
    const BOOL ok = ReadFile(h, &bytes[0], size, &got, NULL);
    CloseHandle(h);
    if (!ok || got == 0)
        return;

    const int need = MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(), (int)got, NULL, 0);
    if (need <= 0)
        return;

    std::wstring text((size_t)need, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(), (int)got, &text[0], need);

    bool nextIsPath = false;
    size_t i = 0;
    while (i < text.size())
    {
        if (text[i] != L'"')
        {
            i++;
            continue;
        }

        i++;
        std::wstring token;
        while (i < text.size() && text[i] != L'"')
        {
            if (text[i] == L'\\' && i + 1 < text.size() && text[i + 1] == L'\\')
            {
                token += L'\\';
                i += 2;
            }
            else
            {
                token += text[i];
                i++;
            }
        }
        if (i < text.size())
            i++;   // the closing quote

        if (nextIsPath)
        {
            out.push_back(token);
            nextIsPath = false;
        }
        else if (_wcsicmp(token.c_str(), L"path") == 0)
        {
            nextIsPath = true;
        }
    }
}

void CollectSteamLibraries(std::vector<std::wstring>& out)
{
    std::vector<std::wstring> steamRoots;

    // SteamPath comes back with forward slashes; InstallPath does not. Both are
    // normalized by AddRoot, but the vdf lookups below need it done here.
    steamRoots.push_back(ReadRegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", 0));
    steamRoots.push_back(ReadRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", KEY_WOW64_32KEY));
    steamRoots.push_back(ReadRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", KEY_WOW64_64KEY));

    const std::wstring pf86 = Env(L"ProgramFiles(x86)");
    if (!pf86.empty())
        steamRoots.push_back(Join(pf86, L"Steam"));

    for (size_t i = 0; i < steamRoots.size(); i++)
    {
        if (steamRoots[i].empty())
            continue;

        const std::wstring root = NoTrailingSlash(NormalizeSlashes(steamRoots[i]));
        out.push_back(root);

        // Steam has moved this file between the two locations over the years and
        // still writes whichever one an older install already had.
        ParseLibraryFoldersVdf(Join(Join(root, L"steamapps"), L"libraryfolders.vdf"), out);
        ParseLibraryFoldersVdf(Join(Join(root, L"config"), L"libraryfolders.vdf"), out);
    }
}

// Everything the sweeper should look at, in the order it should look. Earlier
// roots are the ones a preset pack is most likely to be under and the cheapest to
// walk, so the budget runs out somewhere harmless.
void BuildRoots(Sweeper& sweeper)
{
    const std::wstring profile = Env(L"USERPROFILE");
    const std::wstring pf      = Env(L"ProgramFiles");
    const std::wstring pf86    = Env(L"ProgramFiles(x86)");
    const std::wstring local   = Env(L"LOCALAPPDATA");
    const std::wstring common  = Env(L"ProgramData");

    // Presets dropped next to the executable, which is the layout the app's own
    // default points at.
    sweeper.AddRoot(NoTrailingSlash(g_plugin.m_szMilkdrop2Path), 2, L"Milk Run folder");

    // Winamp, where MilkDrop 2 has always installed: Plugins\Milkdrop2\presets.
    std::vector<std::wstring> winampRoots;
    winampRoots.push_back(ReadRegString(HKEY_CURRENT_USER, L"Software\\Winamp", NULL, 0));
    winampRoots.push_back(ReadRegString(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Winamp",
        L"InstallLocation", KEY_WOW64_32KEY));
    if (!pf86.empty()) winampRoots.push_back(Join(pf86, L"Winamp"));
    if (!pf.empty())   winampRoots.push_back(Join(pf, L"Winamp"));

    for (size_t i = 0; i < winampRoots.size(); i++)
        sweeper.AddRoot(winampRoots[i], 4, L"Winamp");

    // Installed programs, but only the ones whose name says what they are. A blind
    // sweep of Program Files is thousands of directories for one possible hit.
    static const wchar_t* const kInteresting[] = {
        L"milk", L"winamp", L"projectm", L"beatdrop", L"visbot", L"vis_"
    };

    std::vector<std::wstring> programRoots;
    if (!pf.empty())     programRoots.push_back(pf);
    if (!pf86.empty())   programRoots.push_back(pf86);
    if (!common.empty()) programRoots.push_back(common);
    if (!local.empty())
    {
        programRoots.push_back(local);
        programRoots.push_back(Join(local, L"Programs"));
    }

    for (size_t i = 0; i < programRoots.size(); i++)
    {
        WIN32_FIND_DATAW ffd;
        HANDLE h = FindFirstFileW(Join(programRoots[i], L"*").c_str(), &ffd);
        if (h == INVALID_HANDLE_VALUE)
            continue;

        do
        {
            if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                continue;
            if (ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                continue;

            const std::wstring lower = LowerCase(ffd.cFileName);
            for (int k = 0; k < ARRAYSIZE(kInteresting); k++)
            {
                if (lower.find(kInteresting[k]) != std::wstring::npos)
                {
                    sweeper.AddRoot(Join(programRoots[i], ffd.cFileName), 3, L"Installed");
                    break;
                }
            }
        } while (FindNextFileW(h, &ffd));

        FindClose(h);
    }

    // Steam, for projectM. Depth two below steamapps\common is the game folder and
    // then its own directories, which is where a presets folder sits.
    std::vector<std::wstring> steamLibs;
    CollectSteamLibraries(steamLibs);
    for (size_t i = 0; i < steamLibs.size(); i++)
        sweeper.AddRoot(Join(Join(steamLibs[i], L"steamapps"), L"common"), 2, L"Steam");

    // A library the user made themselves and Steam never recorded. One attribute
    // probe per top-level profile folder finds it; a sweep deep enough to stumble
    // into it would cost thousands.
    //
    // Junctions are deliberately included. A profile folder that is really a
    // mounted second disk is where a Steam library goes when C: is full, and that
    // is the exact case this probe is for.
    if (!profile.empty())
    {
        WIN32_FIND_DATAW ffd;
        HANDLE h = FindFirstFileW(Join(profile, L"*").c_str(), &ffd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                    continue;
                if (IsSkippedDirName(ffd.cFileName))
                    continue;

                const std::wstring child = Join(profile, ffd.cFileName);
                sweeper.AddRoot(Join(Join(child, L"steamapps"), L"common"), 2, L"Steam");
                sweeper.AddRoot(Join(Join(Join(child, L"SteamLibrary"), L"steamapps"), L"common"), 2, L"Steam");
            } while (FindNextFileW(h, &ffd));

            FindClose(h);
        }
    }

    // Where a downloaded pack actually lands. SHGetFolderPath rather than the
    // profile plus a name, because Desktop and Documents are commonly redirected
    // into OneDrive and the literal folders are then empty.
    struct KnownFolder { int csidl; const wchar_t* where; const wchar_t* fallback; };
    static const KnownFolder kKnown[] = {
        { CSIDL_DESKTOPDIRECTORY, L"Desktop",   L"Desktop"   },
        { CSIDL_PERSONAL,         L"Documents", L"Documents" },
        { CSIDL_MYMUSIC,          L"Music",     L"Music"     },
        { CSIDL_MYVIDEO,          L"Videos",    L"Videos"    },
    };

    for (int i = 0; i < ARRAYSIZE(kKnown); i++)
    {
        wchar_t buf[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, kKnown[i].csidl, NULL, SHGFP_TYPE_CURRENT, buf)))
            sweeper.AddRoot(buf, 4, kKnown[i].where);

        if (!profile.empty())
            sweeper.AddRoot(Join(profile, kKnown[i].fallback), 4, kKnown[i].where);
    }

    // Downloads has no CSIDL, only a known folder id, and reaching that means a
    // runtime import for one folder. The literal name is right on every Windows
    // this app runs on.
    if (!profile.empty())
        sweeper.AddRoot(Join(profile, L"Downloads"), 4, L"Downloads");

    // OneDrive, personal and any number of work tenants. The env vars cover the
    // signed-in ones; the profile scan covers a tenant that has been unlinked but
    // left its folder behind.
    sweeper.AddRoot(Env(L"OneDrive"), 4, L"OneDrive");
    sweeper.AddRoot(Env(L"OneDriveConsumer"), 4, L"OneDrive");
    sweeper.AddRoot(Env(L"OneDriveCommercial"), 4, L"OneDrive");

    if (!profile.empty())
    {
        WIN32_FIND_DATAW ffd;
        HANDLE h = FindFirstFileW(Join(profile, L"OneDrive*").c_str(), &ffd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                    continue;

                const std::wstring od = Join(profile, ffd.cFileName);
                // The redirected copies first: they are three levels shallower than
                // the same folder reached from the OneDrive root, so a pack under
                // them is found before the budget matters.
                sweeper.AddRoot(Join(od, L"Desktop"), 4, L"OneDrive");
                sweeper.AddRoot(Join(od, L"Documents"), 4, L"OneDrive");
                sweeper.AddRoot(Join(od, L"Downloads"), 4, L"OneDrive");
                sweeper.AddRoot(od, 4, L"OneDrive");
            } while (FindNextFileW(h, &ffd));

            FindClose(h);
        }
    }

    // Last and deepest: everything else the user keeps in their profile. Four
    // levels is what a pack unzipped into a folder of its own actually needs, the
    // release directory and then the product directory above the presets, and this
    // is the root the budget is expected to cut short.
    sweeper.AddRoot(profile, 4, L"Home folder");
}

unsigned __stdcall ScanThread(void* param)
{
    ScanState* st = (ScanState*)param;

    Sweeper sweeper(*st);
    BuildRoots(sweeper);
    sweeper.Run();

    PostMessageW(st->hDlg, WM_FIRSTRUN_SCAN_DONE, 0, 0);
    return 0;
}

/////////////////////////////////////////////////////////////////////////////
// Dialog state

struct FirstRunUi
{
    HINSTANCE          hInstance;
    ScanState          scan;
    HANDLE             thread;

    std::vector<Found> shown;        // sorted copy, what the list holds
    size_t             drained;      // how much of scan.found is already in 'shown'
    std::wstring       selectedPath; // survives the list being rebuilt
    bool               userPicked;   // stop re-selecting the best once they choose
    bool               scanDone;

    // Set while the list is being rebuilt. Restoring the selection sends the same
    // LVN_ITEMCHANGED a click does, and that handler reads a selection as the user
    // having chosen: without this, the first row the scan recommends is also the
    // last, because a better folder arriving later can no longer take its place.
    bool               selecting;

    FirstRunOutcome    outcome;

    FirstRunUi()
        : hInstance(NULL), thread(NULL), drained(0), userPicked(false),
          scanDone(false), selecting(false) {}

private:
    FirstRunUi(const FirstRunUi&);
    FirstRunUi& operator=(const FirstRunUi&);
};

/////////////////////////////////////////////////////////////////////////////
// The list
//
// Every list view message with an A/W split is spelled out. The project does not
// define UNICODE, so ListView_InsertColumn and friends resolve to the ANSI message
// and read a wide string as ANSI: the column header comes out as one character.
// This is the same trap the settings dialog's tab control fell into.

enum Column
{
    kColFolder   = 0,
    kColPresets  = 1,
    kColTextures = 2,
    kColWhere    = 3,
    kColCount    = 4
};

void SizeColumns(HWND list)
{
    RECT rc;
    GetClientRect(list, &rc);

    // Room for a vertical scrollbar whether or not one is up, so adding the last
    // row never makes the columns jump.
    int width = (rc.right - rc.left) - GetSystemMetrics(SM_CXVSCROLL) - 4;
    if (width < 120)
        width = 120;

    // Fractions, not constants: LVM_SETCOLUMNWIDTH is in pixels and this dialog is
    // scaled for whichever monitor it opens on.
    const int presets  = (width * 14) / 100;
    const int textures = (width * 15) / 100;
    const int where    = (width * 22) / 100;

    SendMessageW(list, LVM_SETCOLUMNWIDTH, kColPresets,  MAKELPARAM(presets, 0));
    SendMessageW(list, LVM_SETCOLUMNWIDTH, kColTextures, MAKELPARAM(textures, 0));
    SendMessageW(list, LVM_SETCOLUMNWIDTH, kColWhere,    MAKELPARAM(where, 0));
    SendMessageW(list, LVM_SETCOLUMNWIDTH, kColFolder,
                 MAKELPARAM(width - presets - textures - where, 0));
}

bool CreateColumns(HWND list)
{
    struct ColumnDef { const wchar_t* title; int format; };
    static const ColumnDef kColumns[kColCount] = {
        { L"Folder",   LVCFMT_LEFT  },
        { L"Presets",  LVCFMT_RIGHT },
        { L"Textures", LVCFMT_RIGHT },
        { L"Where",    LVCFMT_LEFT  },
    };

    for (int i = 0; i < kColCount; i++)
    {
        LVCOLUMNW col;
        ZeroMemory(&col, sizeof(col));
        col.mask    = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
        col.fmt     = kColumns[i].format;
        col.cx      = 60;
        col.iSubItem = i;
        col.pszText = (LPWSTR)kColumns[i].title;

        if (SendMessageW(list, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&col) == -1)
            return false;
    }

    SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP | LVS_EX_DOUBLEBUFFER);
    SizeColumns(list);
    return true;
}

void SetCell(HWND list, int row, int column, const wchar_t* text)
{
    LVITEMW item;
    ZeroMemory(&item, sizeof(item));
    item.iItem    = row;
    item.iSubItem = column;
    item.pszText  = (LPWSTR)text;
    SendMessageW(list, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&item);
}

// Rebuilt whole rather than appended to, because the order is by preset count and
// a folder found late can belong at the top. The list never holds more than a
// couple of dozen rows, so this costs nothing worth measuring.
void RebuildList(HWND hDlg, FirstRunUi* ui)
{
    HWND list = GetDlgItem(hDlg, IDC_FR_LIST);
    if (!list)
        return;

    ui->selecting = true;

    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(list, LVM_DELETEALLITEMS, 0, 0);

    // Until they pick a row themselves the selection is a recommendation, and it
    // follows whatever is best so far. Matching the remembered path instead would
    // pin the first folder the scan happened to reach, which is the one found in
    // the cheapest place rather than the one with the most presets.
    int selectRow = (!ui->userPicked && !ui->shown.empty()) ? 0 : -1;

    for (size_t i = 0; i < ui->shown.size(); i++)
    {
        const Found& f = ui->shown[i];

        LVITEMW item;
        ZeroMemory(&item, sizeof(item));
        item.mask     = LVIF_TEXT;
        item.iItem    = (int)i;
        const std::wstring folder = ShortPath(f.path, 3);
        item.pszText  = (LPWSTR)folder.c_str();

        const int row = (int)SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&item);
        if (row < 0)
            continue;

        SetCell(list, row, kColPresets,  Format(L"%d", f.presets).c_str());
        SetCell(list, row, kColTextures, f.textures > 0 ? Format(L"%d", f.textures).c_str() : L"none");
        SetCell(list, row, kColWhere,    f.where);

        if (ui->userPicked && _wcsicmp(f.path.c_str(), ui->selectedPath.c_str()) == 0)
            selectRow = row;
    }

    if (selectRow >= 0)
    {
        LVITEMW state;
        ZeroMemory(&state, sizeof(state));
        state.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        state.state     = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(list, LVM_SETITEMSTATE, (WPARAM)selectRow, (LPARAM)&state);
        SendMessageW(list, LVM_ENSUREVISIBLE, (WPARAM)selectRow, FALSE);
    }

    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, NULL, TRUE);

    ui->selecting = false;
}

int SelectedRow(HWND hDlg)
{
    HWND list = GetDlgItem(hDlg, IDC_FR_LIST);
    if (!list)
        return -1;
    return (int)SendMessageW(list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
}

const Found* Selection(HWND hDlg, FirstRunUi* ui)
{
    const int row = SelectedRow(hDlg);
    if (row < 0 || (size_t)row >= ui->shown.size())
        return NULL;
    return &ui->shown[row];
}

/////////////////////////////////////////////////////////////////////////////
// What the window says about the current choice

void UpdateChoice(HWND hDlg, FirstRunUi* ui)
{
    const Found* f = Selection(hDlg, ui);

    EnableWindow(GetDlgItem(hDlg, IDOK), f != NULL);

    if (!f)
    {
        SetText(hDlg, IDC_FR_CHOSEN,
                ui->scanDone && ui->shown.empty()
                    ? L"Nothing chosen. Press Browse to point Milk Run at a folder of .milk files."
                    : L"Nothing chosen yet.");
        SetText(hDlg, IDC_FR_TEXTURES, L"");
        return;
    }

    ui->selectedPath = f->path;

    // Count first and path last on both lines. The path is the part that has to
    // give when the window is narrower than the text, and SS_PATHELLIPSIS only
    // shortens it if there is nothing after it to lose first.
    SetText(hDlg, IDC_FR_CHOSEN,
            Format(L"Presets     %d in  %s", f->presets, f->path.c_str()).c_str());

    const std::wstring textures = SiblingTextureDir(f->path);
    if (textures.empty())
    {
        // Not an error. A pack without textures is normal, and the presets that do
        // want one fall back to looking in the preset folder itself.
        SetText(hDlg, IDC_FR_TEXTURES,
                L"Textures    none beside this folder. Presets that ask for an image file "
                L"will look in the preset folder instead.");
    }
    else
    {
        SetText(hDlg, IDC_FR_TEXTURES,
                Format(L"Textures    %d, used automatically, in  %s",
                       f->textures, textures.c_str()).c_str());
    }
}

void UpdateStatus(HWND hDlg, FirstRunUi* ui)
{
    const int n = (int)ui->shown.size();

    if (!ui->scanDone)
    {
        SetText(hDlg, IDC_FR_STATUS,
                n == 0 ? L"Searching this PC..."
                       : Format(L"Searching this PC...  %d so far", n).c_str());
        return;
    }

    // The search gave up before it ran out of places to look, so "nothing found"
    // would be a claim it has not earned.
    const bool partial = InterlockedCompareExchange(&ui->scan.cutShort, 0, 0) != 0;

    if (n == 0)
    {
        SetText(hDlg, IDC_FR_STATUS,
                partial ? L"Nothing yet, and the search is out of time. Press Browse."
                        : L"Nothing found. Presets are .milk files; download a pack, unzip "
                          L"it, then press Browse.");
        return;
    }

    SetText(hDlg, IDC_FR_STATUS,
            partial ? Format(L"%d so far, best first. The search stopped there; "
                             L"Browse for the rest.", n).c_str()
                    : Format(L"%d found, most presets first.", n).c_str());
}

// Copies whatever the worker has appended since the last tick. The worker only
// ever appends, so the lock is held for the copy and nothing longer.
bool DrainResults(FirstRunUi* ui)
{
    std::vector<Found> fresh;

    EnterCriticalSection(&ui->scan.lock);
    const size_t have = ui->scan.found.size();
    if (have > ui->drained)
    {
        fresh.assign(ui->scan.found.begin() + ui->drained, ui->scan.found.end());
        ui->drained = have;
    }
    LeaveCriticalSection(&ui->scan.lock);

    if (fresh.empty())
        return false;

    ui->shown.insert(ui->shown.end(), fresh.begin(), fresh.end());
    std::sort(ui->shown.begin(), ui->shown.end(), BetterFirst);
    return true;
}

/////////////////////////////////////////////////////////////////////////////
// Browse

bool BrowseForFolder(HWND parent, const std::wstring& start, std::wstring& chosen)
{
    IFileDialog* dlg = NULL;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                  IID_IFileDialog, (void**)&dlg);
    if (FAILED(hr))
    {
        MessageBoxW(parent,
                    Format(L"The folder picker could not be created (0x%08X).",
                           (unsigned)hr).c_str(),
                    L"Cannot browse", MB_OK | MB_ICONWARNING);
        return false;
    }

    DWORD options = 0;
    if (SUCCEEDED(dlg->GetOptions(&options)))
        dlg->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);

    dlg->SetTitle(L"Choose a folder of .milk presets");

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

void OnBrowse(HWND hDlg, FirstRunUi* ui)
{
    const Found* current = Selection(hDlg, ui);
    const std::wstring start = current ? current->path : std::wstring(g_plugin.m_szPresetDir);

    std::wstring chosen;
    if (!BrowseForFolder(hDlg, start, chosen))
        return;

    chosen = NoTrailingSlash(chosen);

    const int presets = CountMilkFiles(chosen);
    if (presets == 0)
    {
        // The same guard the settings dialog applies to its preset folder, and for
        // the same reason: an empty folder is exactly the state this window exists
        // to get the user out of.
        MessageBoxW(hDlg,
                    Format(L"No .milk presets in that folder:\n\n%s\n\nPresets are plain "
                           L".milk files. If you have just unzipped a pack, the folder you "
                           L"want is usually the one called presets.", chosen.c_str()).c_str(),
                    L"Nothing to load", MB_OK | MB_ICONWARNING);
        return;
    }

    // The scan may already have it. Selecting the existing row keeps one folder to
    // one row, rather than showing the same path twice with different labels.
    for (size_t i = 0; i < ui->shown.size(); i++)
    {
        if (_wcsicmp(ui->shown[i].path.c_str(), chosen.c_str()) == 0)
        {
            ui->userPicked   = true;
            ui->selectedPath = ui->shown[i].path;
            RebuildList(hDlg, ui);
            UpdateChoice(hDlg, ui);
            return;
        }
    }

    Found f;
    f.path     = chosen;
    f.where    = L"Chosen";
    f.presets  = presets;

    const std::wstring textures = SiblingTextureDir(chosen);
    f.textures = textures.empty() ? 0 : CountTextureFiles(textures);

    ui->shown.push_back(f);
    std::sort(ui->shown.begin(), ui->shown.end(), BetterFirst);

    ui->userPicked   = true;
    ui->selectedPath = chosen;

    RebuildList(hDlg, ui);
    UpdateStatus(hDlg, ui);
    UpdateChoice(hDlg, ui);
}

/////////////////////////////////////////////////////////////////////////////
// Applying the choice
//
// Deliberately not run from inside the dialog procedure. UpdatePresetList waits
// for its worker and LoadRandomPreset compiles shaders, which together are long
// enough to leave a window on screen that is not repainting.

void ApplyChoice(HWND parent, FirstRunOutcome& outcome,
                 const std::wstring& presetDir, const std::wstring& textureDir)
{
    wchar_t* pIni = g_plugin.GetConfigIniFile();

    lstrcpynW(g_plugin.m_szPresetDir, WithTrailingSlash(presetDir).c_str(), MAX_PATH);
    lstrcpynW(g_plugin.m_szTextureDir, WithTrailingSlash(textureDir).c_str(), MAX_PATH);

    WritePrivateProfileStringW(L"settings", L"szPresetDir",  g_plugin.m_szPresetDir,  pIni);
    WritePrivateProfileStringW(L"settings", L"szTextureDir", g_plugin.m_szTextureDir, pIni);

    // The texture cache is keyed by name against the old directory, and the loader
    // only re-lists that directory when this is set.
    g_plugin.m_bNeedRescanTexturesDir = true;

    // Not the background early-out the app starts with. That one returns after the
    // first ~32 files and leaves the rest arriving over the next few seconds, which
    // is fine at startup and wrong here: the count this window just promised has to
    // be the count the user gets.
    g_plugin.UpdatePresetList(false, true);

    // The running preset, if any, is not in this list, so its index means nothing.
    g_plugin.m_nCurrentPreset = -1;

    if (g_plugin.m_nPresets - g_plugin.m_nDirs <= 0)
    {
        MessageBoxW(parent,
                    Format(L"Nothing loaded from:\n\n%s\n\nThe folder had presets a moment "
                           L"ago. Check it is still there and reachable.",
                           g_plugin.m_szPresetDir).c_str(),
                    L"Cannot load presets", MB_OK | MB_ICONWARNING);
        return;
    }

    // The six-second toast and the 2003 overlay browser that LoadRandomPreset put
    // up when the folder was empty. Both are stale the moment there is a list.
    g_plugin.ClearErrors(ERR_MISC);
    if (g_plugin.m_UI_mode == UI_LOAD)
        g_plugin.m_UI_mode = UI_REGULAR;

    g_plugin.LoadRandomPreset(0.0f);

    outcome.accepted = true;
}

/////////////////////////////////////////////////////////////////////////////
// Dialog procedure

INT_PTR CALLBACK FirstRunProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    FirstRunUi* ui = (FirstRunUi*)GetWindowLongPtrW(hDlg, DWLP_USER);

    switch (msg)
    {
    case WM_INITDIALOG:
    {
        ui = (FirstRunUi*)lp;
        SetWindowLongPtrW(hDlg, DWLP_USER, (LONG_PTR)ui);

        InheritOwnerIcon(hDlg);

        HWND list = GetDlgItem(hDlg, IDC_FR_LIST);
        if (!list || !CreateColumns(list))
        {
            MessageBoxW(hDlg,
                        L"The preset folder list could not be created. Its resources are "
                        L"missing from this build.",
                        L"Cannot set up presets", MB_OK | MB_ICONERROR);
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }

        ui->scan.hDlg = hDlg;

        // Off the message thread on purpose. The sweep touches thousands of
        // directories and a window that has not painted by the time the user looks
        // at it reads as a hang, not as a search.
        ui->thread = (HANDLE)_beginthreadex(NULL, 0, ScanThread, &ui->scan, 0, NULL);
        if (ui->thread)
        {
            SetTimer(hDlg, kDrainTimer, kDrainMs, NULL);
        }
        else
        {
            // Browse still works, so this is a smaller window rather than no window.
            ui->scanDone = true;
        }

        UpdateStatus(hDlg, ui);
        UpdateChoice(hDlg, ui);
        return TRUE;
    }

    case WM_TIMER:
        if (wp == kDrainTimer && ui)
        {
            if (DrainResults(ui))
            {
                RebuildList(hDlg, ui);
                UpdateChoice(hDlg, ui);
            }
            UpdateStatus(hDlg, ui);
        }
        return TRUE;

    case WM_FIRSTRUN_SCAN_DONE:
        if (ui)
        {
            KillTimer(hDlg, kDrainTimer);
            ui->scanDone = true;
            if (DrainResults(ui))
                RebuildList(hDlg, ui);
            UpdateStatus(hDlg, ui);
            UpdateChoice(hDlg, ui);
        }
        return TRUE;

    case WM_SIZE:
        // Per-monitor v2 resizes the dialog and its controls when it crosses to a
        // monitor at another scale. The column widths are pixels and are not part
        // of that, so they are recomputed from the new client width.
        if (ui)
        {
            HWND list = GetDlgItem(hDlg, IDC_FR_LIST);
            if (list)
                SizeColumns(list);
        }
        return FALSE;

    case WM_CTLCOLORSTATIC:
    {
        const int id = GetDlgCtrlID((HWND)lp);
        if (id == IDC_FR_KEY_RENDER || id == IDC_FR_KEY_SETTINGS)
        {
            SetTextColor((HDC)wp, RGB(0, 90, 158));
            SetBkMode((HDC)wp, TRANSPARENT);
            return (INT_PTR)GetSysColorBrush(COLOR_BTNFACE);
        }
        return FALSE;
    }

    case WM_NOTIFY:
    {
        NMHDR* hdr = (NMHDR*)lp;
        if (!ui || !hdr || hdr->idFrom != IDC_FR_LIST)
            return FALSE;

        if (hdr->code == LVN_ITEMCHANGED)
        {
            NMLISTVIEW* nm = (NMLISTVIEW*)lp;
            if (nm->uChanged & LVIF_STATE)
            {
                if ((nm->uNewState & LVIS_SELECTED) && !ui->selecting)
                    ui->userPicked = true;
                UpdateChoice(hDlg, ui);
            }
            return TRUE;
        }

        if (hdr->code == NM_DBLCLK)
        {
            // A double click on a row is the same statement as picking it and
            // pressing the default button.
            if (Selection(hDlg, ui))
                SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
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
        case IDC_FR_BROWSE:
            OnBrowse(hDlg, ui);
            return TRUE;

        case IDOK:
        {
            const Found* f = Selection(hDlg, ui);
            if (!f)
                return TRUE;

            // Re-counted rather than trusted: the scan may have run minutes ago if
            // the user stopped to read, and a folder on removable media can be gone.
            if (CountMilkFiles(f->path) == 0)
            {
                MessageBoxW(hDlg,
                            Format(L"No .milk presets there any more:\n\n%s",
                                   f->path.c_str()).c_str(),
                            L"Cannot use that folder", MB_OK | MB_ICONWARNING);
                return TRUE;
            }

            ui->outcome.presetCount = f->presets;
            ui->outcome.textureCount = f->textures;
            ui->selectedPath = f->path;
            ui->outcome.dontAskAgain =
                IsDlgButtonChecked(hDlg, IDC_FR_DONT_ASK) == BST_CHECKED;

            EndDialog(hDlg, IDOK);
            return TRUE;
        }

        case IDCANCEL:
            // The checkbox is recorded either way. Someone who wants to be left
            // alone is more likely to say so on the way out than on the way in.
            ui->outcome.dontAskAgain =
                IsDlgButtonChecked(hDlg, IDC_FR_DONT_ASK) == BST_CHECKED;
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    }

    case WM_DESTROY:
        if (ui)
        {
            KillTimer(hDlg, kDrainTimer);
            // The worker checks this between directories, so it unwinds in about
            // the time one FindFirstFile takes.
            InterlockedExchange(&ui->scan.cancel, 1);
        }
        return FALSE;
    }

    return FALSE;
}

} // namespace

bool FirstRunNeeded()
{
    if (GetPrivateProfileBoolW(L"settings", kSkipKey, false, g_plugin.GetConfigIniFile()))
        return false;

    return CountMilkFiles(NoTrailingSlash(g_plugin.m_szPresetDir)) == 0;
}

FirstRunOutcome ShowFirstRunDialog(HINSTANCE hInstance, HWND parent)
{
    ComScope   com;
    ThemeScope theme(hInstance);
    DpiScope   dpi;

    FirstRunUi ui;
    ui.hInstance = hInstance;

    const INT_PTR r = DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_FIRST_RUN),
                                      parent, FirstRunProc, (LPARAM)&ui);
    if (r == -1)
    {
        MessageBoxW(parent,
                    L"The preset setup window could not be created. Its resources are "
                    L"missing from this build.",
                    L"Cannot set up presets", MB_OK | MB_ICONERROR);
        return ui.outcome;
    }

    if (ui.thread)
    {
        // WM_DESTROY has already asked it to stop, so this is a short wait. It
        // exists so the handle is never closed out from under a live thread, and so
        // nothing writes into ui.scan after ui goes out of scope.
        WaitForSingleObject(ui.thread, INFINITE);
        CloseHandle(ui.thread);
        ui.thread = NULL;
    }

    if (ui.outcome.dontAskAgain)
        WritePrivateProfileIntW(1, kSkipKey, g_plugin.GetConfigIniFile(), L"settings");

    if (r == IDOK && !ui.selectedPath.empty())
    {
        std::wstring textures = SiblingTextureDir(ui.selectedPath);

        // A pack that shipped no textures still gets a texture directory written,
        // the app's own. Naming it in the ini beats leaving the key out and relying
        // on a compiled default, which is exactly the kind of invisible setting
        // this window exists to put in front of somebody.
        if (textures.empty())
            textures = Join(NoTrailingSlash(g_plugin.m_szMilkdrop2Path), L"textures");

        ApplyChoice(parent, ui.outcome, ui.selectedPath, textures);
    }

    return ui.outcome;
}

} // namespace app
