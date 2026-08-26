#ifndef __MILKRUN_RENDER_DIALOG_IDS_H__
#define __MILKRUN_RENDER_DIALOG_IDS_H__ 1

// Deliberately not in resource.h. That file is generated territory shared with
// the visualizer's 590-odd strings, and the 32000 block is clear of everything
// it uses, so the two can never collide.

// resource.h defines this too, but render_dialog.rc has to stand on its own when
// it is compiled outside the visualizer's resource script.
#ifndef IDC_STATIC
#define IDC_STATIC                      (-1)
#endif

#define IDD_RENDER_SETTINGS             32000
#define IDD_RENDER_PROGRESS             32001

// Side-by-side manifest for the version 6 common controls, activated only around
// dialog creation. Resource id 1 is what the loader applies process-wide, so this
// is 2: it themes these dialogs without changing anything else about the app.
#define IDR_RENDER_DIALOG_MANIFEST      32002

// Settings dialog.
#define IDC_RD_PRESET                   32010
#define IDC_RD_PRESET_BROWSE            32011
#define IDC_RD_AUDIO                    32012
#define IDC_RD_AUDIO_BROWSE             32013
#define IDC_RD_OUTPUT                   32014
#define IDC_RD_OUTPUT_BROWSE            32015

#define IDC_RD_SIZE                     32020
#define IDC_RD_WIDTH                    32021
#define IDC_RD_HEIGHT                   32022
#define IDC_RD_FPS                      32023
#define IDC_RD_FPS_HINT                 32024

#define IDC_RD_QUALITY                  32030
#define IDC_RD_QUALITY_TEXT             32031

#define IDC_RD_PREC_FAITHFUL            32040
#define IDC_RD_PREC_HIGH                32041
#define IDC_RD_PQ2020                   32042
#define IDC_RD_ENCODER                  32043

#define IDC_RD_SECTION                  32050
#define IDC_RD_START                    32051
#define IDC_RD_DURATION                 32052
#define IDC_RD_START_LABEL              32053
#define IDC_RD_DURATION_LABEL           32054
#define IDC_RD_SECTION_HINT             32055

#define IDC_RD_SUMMARY                  32060
#define IDC_RD_WARNING                  32061

// Progress dialog.
#define IDC_RP_HEADING                  32070
#define IDC_RP_FILE                     32071
#define IDC_RP_BAR                      32072
#define IDC_RP_FRAMES                   32073
#define IDC_RP_RATE                     32074
#define IDC_RP_ELAPSED                  32075
#define IDC_RP_REMAIN                   32076
#define IDC_RP_PLAY                     32077

#endif
