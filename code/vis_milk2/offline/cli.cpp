#include "cli.h"
#include "render_job.h"
#include "render_config.h"
#include "audio_source.h"
#include "player_window.h"

#include <shellapi.h>
#include <stdio.h>
#include <string>

#pragma comment(lib, "shell32.lib")

namespace offline {

namespace {

// The app is built as a windows-subsystem binary, so it owns no console of its
// own. Three cases have to work: launched from a console, launched with stdout
// redirected to a pipe or file, and launched from a GUI with no stdout at all.
//
// Writing through the handle directly rather than the CRT keeps all three honest.
// Reopening CONOUT$, the usual trick, would defeat redirection and make the tool
// unscriptable.
HANDLE g_out = NULL;
bool   g_outIsConsole = false;

void OpenConsole()
{
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);

    if (g_out == NULL || g_out == INVALID_HANDLE_VALUE)
    {
        if (AttachConsole(ATTACH_PARENT_PROCESS))
            g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    }

    if (g_out == INVALID_HANDLE_VALUE)
        g_out = NULL;

    DWORD mode = 0;
    g_outIsConsole = (g_out != NULL) && GetConsoleMode(g_out, &mode) != 0;
}

void Out(const wchar_t* fmt, ...)
{
    if (!g_out)
        return;

    wchar_t buf[2048];
    va_list args;
    va_start(args, fmt);
    const int n = _vsnwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, fmt, args);
    va_end(args);
    if (n <= 0)
        return;

    DWORD written = 0;
    if (g_outIsConsole)
    {
        WriteConsoleW(g_out, buf, (DWORD)n, &written, NULL);
    }
    else
    {
        // A pipe or file: emit UTF-8, which is what anything downstream expects.
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, buf, n, NULL, 0, NULL, NULL);
        if (bytes > 0)
        {
            std::string utf8((size_t)bytes, '\0');
            WideCharToMultiByte(CP_UTF8, 0, buf, n, &utf8[0], bytes, NULL, NULL);
            WriteFile(g_out, utf8.data(), (DWORD)bytes, &written, NULL);
        }
    }
}

bool Matches(const wchar_t* arg, const wchar_t* name)
{
    return _wcsicmp(arg, name) == 0;
}

// Accepts "60" or "24000/1001".
bool ParseFps(const wchar_t* s, int& num, int& den)
{
    const wchar_t* slash = wcschr(s, L'/');
    if (slash)
    {
        num = _wtoi(std::wstring(s, slash - s).c_str());
        den = _wtoi(slash + 1);
    }
    else
    {
        // Allow a decimal like 29.97 by scaling to a rational.
        if (wcschr(s, L'.'))
        {
            double v = _wtof(s);
            num = (int)(v * 1000.0 + 0.5);
            den = 1000;
        }
        else
        {
            num = _wtoi(s);
            den = 1;
        }
    }
    return num > 0 && den > 0;
}

bool ParseSize(const wchar_t* s, int& w, int& h)
{
    const wchar_t* x = wcspbrk(s, L"xX*");
    if (!x)
        return false;
    w = _wtoi(std::wstring(s, x - s).c_str());
    h = _wtoi(x + 1);
    return w > 0 && h > 0;
}

void PrintUsage()
{
    Out(L"\n");
    Out(L"MilkRun -- render a MilkDrop preset to video, in sync with a song.\n");
    Out(L"\n");
    Out(L"  MilkRun.exe --render --preset <file.milk> --audio <song> --out <file.mp4>\n");
    Out(L"              [--size WxH] [--fps N] [--precision faithful|high]\n");
    Out(L"              [--encoder auto|nvenc|mf] [--pq2020] [--quality N] [--bitrate Kbps]\n");
    Out(L"\n");
    Out(L"  --size        output dimensions, default 1920x1080\n");
    Out(L"  --fps         frame rate; accepts 60, 24000/1001 or 29.97. Default 60\n");
    Out(L"  --precision   faithful (8-bit, matches the live look) or high (float canvas,\n");
    Out(L"                10-bit back buffer). Default faithful\n");
    Out(L"  --pq2020      HDR10 output: BT.2020 primaries with the PQ curve. Requires --precision high\n");
    Out(L"  --encoder     which HEVC encoder to use. Default auto\n");
    Out(L"  --start S     begin S seconds into the song, for auditioning a section\n");
    Out(L"  --duration S  render only S seconds\n");
    Out(L"  --play        open the finished file in the built-in player\n");
    Out(L"  --frames N    render N frames instead of following the song. For smoke tests\n");
    Out(L"  --dump-frames <dir>  also write frames out as PNGs\n");
    Out(L"  --dump-stride N      dump every Nth frame, default every one\n");

    Out(L"\n");
    Out(L"Output is always H.265 Main10, at exactly the requested size and rate, for exactly\n");
    Out(L"as long as the song, with no overlays.\n");
    Out(L"\n");
}

bool ProgressToConsole(void* /*ctx*/, const RenderProgress& p)
{
    static long long lastShown = -1;
    const long long pct = p.frameCount ? (p.frameIndex * 100 / p.frameCount) : 0;
    const bool last = (p.frameIndex == p.frameCount);

    // A console can be redrawn in place; a pipe or log file wants discrete lines,
    // so throttle those to every 10% instead of every 1%.
    const long long step = g_outIsConsole ? 1 : 10;
    if (pct / step == lastShown / step && !last)
        return true;

    lastShown = pct;
    const double rate = p.elapsedSeconds > 0 ? p.frameIndex / p.elapsedSeconds : 0.0;
    Out(g_outIsConsole ? L"\r  %lld / %lld frames (%lld%%)  %.1f fps "
                       : L"  %lld / %lld frames (%lld%%)  %.1f fps\n",
        p.frameIndex, p.frameCount, pct, rate);
    return true;   // the CLI never cancels
}

} // namespace

// Prints the peak sample in each frame's audio window. Feed it a file with a known
// transient and the output says exactly which frame that transient lands on, which
// is the sync question stated directly rather than inferred from pixels.
int ProbeAudio(const std::wstring& path, int fpsNum, int fpsDen,
               long long fromFrame, long long toFrame)
{
    AudioSource audio;
    std::wstring err;
    if (!audio.Open(path, err))
    {
        Out(L"MilkRun: %s\n", err.c_str());
        return 1;
    }

    const long long total = audio.FrameCount(fpsNum, fpsDen);
    Out(L"  duration %.6fs, %lld frames at %.4f fps\n\n",
        audio.DurationSeconds(), total, (double)fpsNum / (double)fpsDen);
    Out(L"  frame    window start (s)    peak L   peak R\n");

    if (toFrame <= 0 || toFrame >= total) toFrame = total - 1;

    unsigned char l[AudioSource::kWindowSamples];
    unsigned char r[AudioSource::kWindowSamples];

    for (long long f = 0; f <= toFrame; f++)
    {
        if (!audio.FillFrameWindow(f, fpsNum, fpsDen, l, r))
        {
            Out(L"  failed to read the window for frame %lld\n", f);
            return 1;
        }
        if (f < fromFrame)
            continue;   // windows must be read in order, so walk but do not print

        int pl = 0, pr = 0;
        for (int i = 0; i < AudioSource::kWindowSamples; i++)
        {
            const int sl = abs((int)(signed char)l[i]);
            const int sr = abs((int)(signed char)r[i]);
            if (sl > pl) pl = sl;
            if (sr > pr) pr = sr;
        }
        Out(L"  %5lld    %14.6f    %6d   %6d\n",
            f, (double)f * fpsDen / (double)fpsNum, pl, pr);
    }
    return 0;
}
bool TryRunCommandLine(HINSTANCE hInstance, int& exitCode)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return false;

    bool wantsRender = false;
    bool wantsHelp   = false;
    bool wantsProbe  = false;
    for (int i = 1; i < argc; i++)
    {
        if (Matches(argv[i], L"--render"))                                  wantsRender = true;
        if (Matches(argv[i], L"--probe-audio"))                             wantsProbe  = true;
        if (Matches(argv[i], L"--help") || Matches(argv[i], L"-h") ||
            Matches(argv[i], L"/?"))                                        wantsHelp = true;
    }

    if (!wantsRender && !wantsHelp && !wantsProbe)
    {
        LocalFree(argv);
        return false;
    }

    OpenConsole();

    if (wantsHelp)
    {
        PrintUsage();
        LocalFree(argv);
        exitCode = 0;
        return true;
    }

    if (wantsProbe)
    {
        std::wstring path;
        int pn = 60, pd = 1;
        long long from = 0, to = 0;
        for (int i = 1; i < argc; i++)
        {
            const bool hasNext = (i + 1 < argc);
            if (Matches(argv[i], L"--probe-audio") && hasNext) { path = argv[i + 1]; i++; }
            else if (Matches(argv[i], L"--fps") && hasNext)    { ParseFps(argv[i + 1], pn, pd); i++; }
            else if (Matches(argv[i], L"--from") && hasNext)   { from = _wtoi64(argv[i + 1]); i++; }
            else if (Matches(argv[i], L"--to") && hasNext)     { to   = _wtoi64(argv[i + 1]); i++; }
        }
        LocalFree(argv);
        exitCode = ProbeAudio(path, pn, pd, from, to);
        return true;
    }

    RenderJobConfig cfg;
    bool wantsPlay = false;
    std::wstring badArg;

    for (int i = 1; i < argc && badArg.empty(); i++)
    {
        const wchar_t* a = argv[i];
        const bool hasNext = (i + 1 < argc);
        const wchar_t* next = hasNext ? argv[i + 1] : L"";

        if      (Matches(a, L"--render"))    continue;
        else if (Matches(a, L"--preset") && hasNext) { cfg.presetPath = next; i++; }
        else if (Matches(a, L"--audio")  && hasNext) { cfg.audioPath  = next; i++; }
        else if (Matches(a, L"--out")    && hasNext) { cfg.outputPath = next; i++; }
        else if (Matches(a, L"--size")   && hasNext)
        {
            if (!ParseSize(next, cfg.width, cfg.height)) badArg = L"--size (expected WxH)";
            i++;
        }
        else if (Matches(a, L"--fps") && hasNext)
        {
            if (!ParseFps(next, cfg.fpsNum, cfg.fpsDen)) badArg = L"--fps";
            i++;
        }
        else if (Matches(a, L"--frames") && hasNext)  { cfg.frameCountOverride = _wtoi64(next); i++; }
        else if (Matches(a, L"--quality") && hasNext) { cfg.quality = _wtoi(next); i++; }
        else if (Matches(a, L"--bitrate") && hasNext) { cfg.bitrateKbps = _wtoi(next); i++; }
        else if (Matches(a, L"--seed") && hasNext)    { cfg.randomSeed = (unsigned)_wtoi(next); i++; }
        else if (Matches(a, L"--pq2020"))            { cfg.pq2020 = true; }
        else if (Matches(a, L"--play"))              { wantsPlay = true; }
        else if (Matches(a, L"--start") && hasNext)    { cfg.startSeconds    = _wtof(next); i++; }
        else if (Matches(a, L"--duration") && hasNext) { cfg.durationSeconds = _wtof(next); i++; }
        else if (Matches(a, L"--dump-frames") && hasNext) { cfg.dumpFramesDir = next; i++; }
        else if (Matches(a, L"--dump-stride") && hasNext) { cfg.dumpFrameStride = _wtoi(next); i++; }
        else if (Matches(a, L"--precision") && hasNext)
        {
            if      (Matches(next, L"faithful")) cfg.precision = Precision::Faithful;
            else if (Matches(next, L"high"))     cfg.precision = Precision::High;
            else badArg = L"--precision (expected faithful or high)";
            i++;
        }
        else if (Matches(a, L"--encoder") && hasNext)
        {
            if      (Matches(next, L"auto"))  cfg.backend = EncoderBackend::Auto;
            else if (Matches(next, L"nvenc")) cfg.backend = EncoderBackend::Nvenc;
            else if (Matches(next, L"mf"))    cfg.backend = EncoderBackend::MediaFoundation;
            else badArg = L"--encoder (expected auto, nvenc or mf)";
            i++;
        }
        else badArg = a;
    }

    LocalFree(argv);

    if (!badArg.empty())
    {
        Out(L"MilkRun: unrecognized or malformed argument: %s\n", badArg.c_str());
        Out(L"Run with --help for usage.\n");
        exitCode = 2;
        return true;
    }

    if (cfg.pq2020 && cfg.precision != Precision::High)
    {
        Out(L"MilkRun: --pq2020 needs --precision high; the conversion requires the float canvas.\n");
        exitCode = 2;
        return true;
    }

    Out(L"MilkRun: rendering %dx%d @ %.4f fps\n",
        cfg.width, cfg.height, (double)cfg.fpsNum / (double)cfg.fpsDen);

    RenderResult r = RunRenderJob(hInstance, cfg, ProgressToConsole, NULL);

    Out(L"\n");
    if (r.ok)
    {
        Out(L"MilkRun: wrote %lld frames in %.1fs (%.1f fps).\n",
            r.framesWritten, r.elapsedSeconds,
            r.elapsedSeconds > 0 ? r.framesWritten / r.elapsedSeconds : 0.0);
        if (!r.warning.empty())
            Out(L"MilkRun: %s\n", r.warning.c_str());
        exitCode = 0;

        if (wantsPlay)
        {
            // A player that will not open does not undo a render that succeeded, so
            // the exit code stays zero and the file stays on disk.
            std::wstring playError;
            if (!PlayFile(hInstance, cfg.outputPath, cfg.fpsNum, cfg.fpsDen, playError))
                Out(L"MilkRun: %s\n", playError.c_str());
        }
    }
    else
    {
        Out(L"MilkRun: render failed: %s\n", r.error.c_str());
        exitCode = 1;
    }
    return true;
}

} // namespace offline
