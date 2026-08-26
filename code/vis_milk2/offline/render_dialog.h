#ifndef __MILKRUN_RENDER_DIALOG_H__
#define __MILKRUN_RENDER_DIALOG_H__ 1

#include <windows.h>
#include "render_config.h"

namespace offline {

// The settings from the last render the user confirmed, or the plain defaults
// before there has been one. Seeding the dialog with these is what stops a second
// render from starting over.
const RenderJobConfig& LastRenderConfig();

// Collects a whole RenderJobConfig from the user and validates it. cfg goes in as
// the starting values and comes back filled in. Returns false when they cancelled;
// when it returns true, cfg has passed every check the dialog can make without
// starting the render, and has been recorded as the last config.
//
// currentPresetPath seeds the preset field when cfg does not already name one, so
// the obvious thing to render is the thing already on screen. Pass NULL when
// nothing is loaded.
bool ShowRenderSettingsDialog(HINSTANCE hInstance, HWND parent,
                              const wchar_t* currentPresetPath,
                              RenderJobConfig& cfg);

// Runs cfg on a worker thread behind a modal progress dialog, so the window keeps
// painting and Cancel keeps working for the length of the render. Returns true
// only when the file was written; 'cancelled' separates the user stopping it from
// the render going wrong, which read the same from the return value alone.
//
// RunRenderJob drives the same CPlugin and the same D3D9 globals the interactive
// session uses, so the caller must have torn the interactive session down first.
bool RunRenderWithProgress(HINSTANCE hInstance, HWND parent,
                           const RenderJobConfig& cfg,
                           bool* cancelled = NULL);

} // namespace offline

#endif
