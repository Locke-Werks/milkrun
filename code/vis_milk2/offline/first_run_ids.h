#ifndef __MILKRUN_FIRST_RUN_IDS_H__
#define __MILKRUN_FIRST_RUN_IDS_H__ 1

// Same reasoning as render_dialog_ids.h and settings_dialog_ids.h: deliberately
// not in resource.h, which is generated territory shared with the visualizer's
// 590-odd strings. render_dialog took 32000-32099 and settings_dialog took
// 32100-32199, so this block starts at 32200 and the three can never collide.

// resource.h defines this too, but first_run.rc has to stand on its own when it
// is compiled outside the visualizer's resource script.
#ifndef IDC_STATIC
#define IDC_STATIC                      (-1)
#endif

// The version 6 common controls manifest lives in render_dialog.rc as
// IDR_RENDER_DIALOG_MANIFEST. There is deliberately no second copy here: one
// activation context resource serves every dialog in the app.

#define IDD_FIRST_RUN                   32200

#define IDC_FR_LIST                     32210
#define IDC_FR_STATUS                   32211
#define IDC_FR_BROWSE                   32212
#define IDC_FR_CHOSEN                   32213
#define IDC_FR_TEXTURES                 32214

// Drawn in an accent colour by WM_CTLCOLORSTATIC. The two shortcuts are the
// other half of what a new user cannot find, so they are not allowed to read as
// grey body text.
#define IDC_FR_KEY_RENDER               32215
#define IDC_FR_KEY_SETTINGS             32216

#define IDC_FR_DONT_ASK                 32217

#endif
