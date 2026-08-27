#ifndef __MILKRUN_RENDER_LOG_H__
#define __MILKRUN_RENDER_LOG_H__ 1

#include <string>
#include "render_config.h"

namespace offline {

// Writes a plain text record of one render beside its output file, as
// <output>.log.
//
// It exists because a render that produced the wrong thing is otherwise very
// hard to reason about after the fact: the file plays, it is the right length,
// and nothing anywhere says which preset actually went into it. A preset that
// was silently substituted looked exactly like a correct render.
//
// So the log records what was asked for, what was actually used, and what the
// machine did with it. The two are separate on purpose: the interesting failures
// are the ones where they differ.
class RenderLog
{
public:
    RenderLog();
    ~RenderLog();

    // Fails quietly. A log that cannot be written must never take the render
    // down with it.
    void Begin(const std::wstring& outputPath, const RenderJobConfig& cfg);

    // Free-form line, timestamped.
    void Line(const wchar_t* fmt, ...);

    // Records the preset the plugin ended up with, which is the check that would
    // have caught a substituted preset.
    void RecordPresetInUse(const wchar_t* presetPath, const wchar_t* presetName);

    void RecordEncoder(const wchar_t* name, bool tenBitInput, bool pq2020);
    void RecordAudio(const wchar_t* path, double durationSeconds, int sampleRate, bool viaWavReader);

    void End(bool ok, long long framesWritten, double elapsedSeconds,
             const std::wstring& error, const std::wstring& warning);

    bool IsOpen() const { return m_file != NULL; }
    const std::wstring& Path() const { return m_path; }

private:
    void Write(const std::wstring& text);

    void*        m_file;   // HANDLE
    std::wstring m_path;
};

} // namespace offline

#endif
