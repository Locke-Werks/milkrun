#ifndef __MILKRUN_SETTINGS_DIALOG_H__
#define __MILKRUN_SETTINGS_DIALOG_H__ 1

#include <windows.h>

namespace app {

// What the dialog did, so the caller can react without re-reading milk2.ini.
struct SettingsOutcome
{
    bool accepted;          // false when the user cancelled; nothing was written
    bool presetDirChanged;  // the dialog has already rescanned the new folder
    bool restartRequired;   // adapter or canvas changed; those need a fresh device

    SettingsOutcome() : accepted(false), presetDirChanged(false), restartRequired(false) {}
};

// Collects the app-level settings from the user and applies them.
//
// Reads the current values on open, writes milk2.ini and updates the live CPlugin
// on OK, does nothing at all on Cancel. Settings that need a DX9 realloc are
// written to the ini and take effect on the next run; the dialog says so.
//
// This is not the in-app overlay menu. It is a modal window that runs on the
// visualizer's message thread while rendering is paused behind it.
SettingsOutcome ShowSettingsDialog(HINSTANCE hInstance, HWND parent);

} // namespace app

#endif
