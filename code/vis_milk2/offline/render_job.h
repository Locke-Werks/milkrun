#ifndef __MILKRUN_RENDER_JOB_H__
#define __MILKRUN_RENDER_JOB_H__ 1

#include <windows.h>
#include <string>
#include "render_config.h"

namespace offline {

struct RenderProgress
{
    long long frameIndex;     // frames completed so far
    long long frameCount;     // total frames this job will produce
    double    elapsedSeconds; // wall clock since the job started
};

// Return false from a progress callback to cancel the render.
typedef bool (*ProgressFn)(void* ctx, const RenderProgress& p);

struct RenderResult
{
    bool         ok = false;
    std::wstring error;        // human-readable, empty when ok
    // Set when the render finished but not in the form asked for, such as an
    // encoder that would only take 8-bit when Main10 was requested. The file is
    // usable; the user just has to be told what they actually got.
    std::wstring warning;

    long long    framesWritten = 0;
    double       elapsedSeconds = 0.0;
};

// Runs one offline render start to finish on the calling thread.
//
// This drives the same CPlugin the interactive app uses, so it must not run while
// an interactive session is live: it puts the plugin into offline mode, creates a
// hidden window and its own device, renders every frame at a fixed timestep, and
// tears the plugin back down.
RenderResult RunRenderJob(HINSTANCE hInstance,
                          const RenderJobConfig& cfg,
                          ProgressFn onProgress = nullptr,
                          void* progressCtx = nullptr);

} // namespace offline

#endif
