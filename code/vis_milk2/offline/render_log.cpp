#include "render_log.h"

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

namespace offline {

namespace {

std::wstring Stamp()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t buf[32];
    swprintf_s(buf, L"%02d:%02d:%02d.%03d  ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return buf;
}

const wchar_t* PrecisionName(Precision p)
{
    return (p == Precision::High) ? L"high (float canvas, 10-bit back buffer)"
                                  : L"faithful (8-bit, matches the live look)";
}

const wchar_t* BackendName(EncoderBackend b)
{
    switch (b)
    {
    case EncoderBackend::Nvenc:           return L"nvenc (forced)";
    case EncoderBackend::MediaFoundation: return L"mf (forced)";
    default:                              return L"auto";
    }
}

} // namespace

RenderLog::RenderLog() : m_file(NULL) {}

RenderLog::~RenderLog()
{
    if (m_file)
    {
        CloseHandle((HANDLE)m_file);
        m_file = NULL;
    }
}

void RenderLog::Write(const std::wstring& text)
{
    if (!m_file)
        return;

    // UTF-8 so preset names with non-ASCII characters survive, which plenty of
    // the community library have.
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                          NULL, 0, NULL, NULL);
    if (bytes <= 0)
        return;

    std::string utf8((size_t)bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &utf8[0], bytes, NULL, NULL);

    DWORD written = 0;
    WriteFile((HANDLE)m_file, utf8.data(), (DWORD)bytes, &written, NULL);
}

void RenderLog::Begin(const std::wstring& outputPath, const RenderJobConfig& cfg)
{
    if (outputPath.empty())
        return;

    m_path = outputPath + L".log";

    HANDLE h = CreateFileW(m_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;   // never fatal
    m_file = h;

    wchar_t buf[2048];
    const double fps = (cfg.fpsDen > 0) ? (double)cfg.fpsNum / (double)cfg.fpsDen : 0.0;

    Write(L"Milk Run render log\r\n");
    Write(L"===================\r\n\r\n");

    Write(L"Asked for\r\n");
    swprintf_s(buf, L"  preset      %s\r\n", cfg.presetPath.c_str());          Write(buf);
    swprintf_s(buf, L"  audio       %s\r\n", cfg.audioPath.c_str());           Write(buf);
    swprintf_s(buf, L"  output      %s\r\n", outputPath.c_str());              Write(buf);
    swprintf_s(buf, L"  size        %d x %d\r\n", cfg.width, cfg.height);      Write(buf);
    swprintf_s(buf, L"  frame rate  %d/%d  (%.4f fps)\r\n", cfg.fpsNum, cfg.fpsDen, fps); Write(buf);
    swprintf_s(buf, L"  precision   %s\r\n", PrecisionName(cfg.precision));    Write(buf);
    swprintf_s(buf, L"  hdr10       %s\r\n", cfg.pq2020 ? L"yes, BT.2020 with the PQ curve" : L"no"); Write(buf);
    swprintf_s(buf, L"  encoder     %s\r\n", BackendName(cfg.backend));        Write(buf);

    if (cfg.bitrateKbps > 0)
        swprintf_s(buf, L"  rate        %d kbps target\r\n", cfg.bitrateKbps);
    else
        swprintf_s(buf, L"  rate        constant quality %d\r\n", cfg.quality);
    Write(buf);

    if (cfg.startSeconds > 0.0 || cfg.durationSeconds > 0.0)
    {
        swprintf_s(buf, L"  section     from %.3fs", cfg.startSeconds);
        Write(buf);
        if (cfg.durationSeconds > 0.0)
            swprintf_s(buf, L" for %.3fs\r\n", cfg.durationSeconds);
        else
            swprintf_s(buf, L" to the end\r\n");
        Write(buf);
    }
    swprintf_s(buf, L"  seed        %u\r\n", cfg.randomSeed); Write(buf);

    Write(L"\r\nWhat actually ran\r\n");
}

void RenderLog::Line(const wchar_t* fmt, ...)
{
    if (!m_file)
        return;

    wchar_t buf[2048];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, fmt, args);
    va_end(args);

    Write(Stamp() + buf + L"\r\n");
}

void RenderLog::RecordPresetInUse(const wchar_t* presetPath, const wchar_t* presetName)
{
    // The line that matters. If this disagrees with "asked for" above, the render
    // is of a different preset than the one requested, which is exactly the
    // failure this log was added for.
    Line(L"preset in use   %s", presetPath ? presetPath : L"(none)");
    if (presetName && presetName[0])
        Line(L"preset name     %s", presetName);
}

void RenderLog::RecordEncoder(const wchar_t* name, bool tenBitInput, bool pq2020)
{
    Line(L"encoder         %s", name ? name : L"(unknown)");
    Line(L"encoder input   %s", tenBitInput ? L"10-bit ARGB10" : L"8-bit ARGB");
    if (pq2020)
        Line(L"colour          BT.2020 primaries, SMPTE ST 2084 transfer");
}

void RenderLog::RecordAudio(const wchar_t* path, double durationSeconds, int sampleRate, bool viaWavReader)
{
    Line(L"audio in use    %s", path ? path : L"(none)");
    Line(L"audio duration  %.6fs", durationSeconds);
    Line(L"audio decoded   %s at %d Hz",
         viaWavReader ? L"by the built-in WAV reader" : L"by Media Foundation", sampleRate);
}

void RenderLog::End(bool ok, long long framesWritten, double elapsedSeconds,
                    const std::wstring& error, const std::wstring& warning)
{
    if (!m_file)
        return;

    wchar_t buf[2048];
    Write(L"\r\nResult\r\n");
    swprintf_s(buf, L"  status      %s\r\n", ok ? L"ok" : L"FAILED"); Write(buf);
    swprintf_s(buf, L"  frames      %lld\r\n", framesWritten); Write(buf);
    swprintf_s(buf, L"  elapsed     %.1fs (%.1f fps)\r\n", elapsedSeconds,
               elapsedSeconds > 0 ? framesWritten / elapsedSeconds : 0.0); Write(buf);

    if (!warning.empty())
    {
        swprintf_s(buf, L"  warning     %s\r\n", warning.c_str());
        Write(buf);
    }
    if (!error.empty())
    {
        swprintf_s(buf, L"  error       %s\r\n", error.c_str());
        Write(buf);
    }

    CloseHandle((HANDLE)m_file);
    m_file = NULL;
}

} // namespace offline
