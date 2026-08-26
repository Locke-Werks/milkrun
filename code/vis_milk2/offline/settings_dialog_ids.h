#ifndef __MILKRUN_SETTINGS_DIALOG_IDS_H__
#define __MILKRUN_SETTINGS_DIALOG_IDS_H__ 1

// Same reasoning as render_dialog_ids.h: deliberately not in resource.h, which is
// generated territory shared with the visualizer's 590-odd strings. render_dialog
// took 32000-32099, so this block starts at 32100 and the three can never collide.

// resource.h defines this too, but settings_dialog.rc has to stand on its own when
// it is compiled outside the visualizer's resource script.
#ifndef IDC_STATIC
#define IDC_STATIC                      (-1)
#endif

// The version 6 common controls manifest lives in render_dialog.rc as
// IDR_RENDER_DIALOG_MANIFEST. There is deliberately no second copy here: one
// activation context resource serves every dialog in the app.

#define IDD_SETTINGS                    32100
#define IDD_SETTINGS_PRESETS            32101
#define IDD_SETTINGS_DISPLAY            32102
#define IDD_SETTINGS_OVERLAY            32103

// Frame.
#define IDC_SD_TABS                     32105
#define IDC_SD_RESET                    32106
#define IDC_SD_WARNING                  32107

// Presets page.
#define IDC_SD_PRESET_DIR               32110
#define IDC_SD_PRESET_DIR_BROWSE        32111
#define IDC_SD_LOCK_AT_STARTUP          32112

#define IDC_SD_TIME_BETWEEN             32115
#define IDC_SD_TIME_RAND                32116
#define IDC_SD_BLEND_AUTO               32117
#define IDC_SD_BLEND_USER               32118

#define IDC_SD_HARD_CUTS                32120
#define IDC_SD_HARD_THRESH              32121
#define IDC_SD_HARD_HALFLIFE            32122
#define IDC_SD_HARD_THRESH_LABEL        32123
#define IDC_SD_HARD_HALFLIFE_LABEL      32124
#define IDC_SD_HARD_HINT                32125

// Display page.
#define IDC_SD_ADAPTER                  32130
#define IDC_SD_MAX_FPS                  32131
#define IDC_SD_SAVE_CPU                 32132

#define IDC_SD_TEX_BITS                 32135
#define IDC_SD_TEX_BITS_HINT            32136
#define IDC_SD_TEX_SIZE                 32137
#define IDC_SD_MESH_X                   32138
#define IDC_SD_MESH_Y                   32139

#define IDC_SD_RESTART_NOTE             32140

// On-screen page.
#define IDC_SD_SHOW_FPS                 32145
#define IDC_SD_SHOW_RATING              32146
#define IDC_SD_SHOW_PRESET_INFO         32147
#define IDC_SD_SHOW_SONG_TITLE          32148
#define IDC_SD_SONG_TITLE_ANIMS         32149

#endif
