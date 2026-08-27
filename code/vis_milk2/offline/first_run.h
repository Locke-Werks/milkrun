#ifndef __MILKRUN_FIRST_RUN_H__
#define __MILKRUN_FIRST_RUN_H__ 1

#include <windows.h>

namespace app {

// What the dialog did, so the caller can react without re-reading milk2.ini.
struct FirstRunOutcome
{
    bool accepted;        // a preset folder was chosen, applied and persisted
    bool dontAskAgain;    // the checkbox, recorded whether or not they accepted
    int  presetCount;     // .milk files in the folder that was applied
    int  textureCount;    // texture files in the sibling folder, 0 when there is none

    FirstRunOutcome() : accepted(false), dontAskAgain(false), presetCount(0), textureCount(0) {}
};

// True when the configured preset folder holds no .milk files and the user has
// not asked to be left alone about it. Cheap: one directory scan.
//
// Reads g_plugin.m_szPresetDir and the ini path, so it must not be called before
// PluginPreInitialize has built them.
bool FirstRunNeeded();

// Offers the user a preset folder, because Milk Run ships none and an empty folder
// is a dead end: the visualizer's own response is a six-second toast and the 2003
// overlay browser, neither of which says where presets come from.
//
// Finds the preset folders already on the machine on a worker thread, takes one,
// applies it to the live CPlugin, writes szPresetDir and szTextureDir to
// milk2.ini, rescans and loads a preset. Does nothing but record the checkbox
// when they decline.
//
// This is not the in-app overlay menu. It is a modal window that runs on the
// visualizer's message thread while rendering is paused behind it, and it needs
// the D3D device to be live, because accepting loads a preset.
FirstRunOutcome ShowFirstRunDialog(HINSTANCE hInstance, HWND parent);

} // namespace app

#endif
