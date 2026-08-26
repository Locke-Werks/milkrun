#ifndef __MILKRUN_APP_DEVICE_H__
#define __MILKRUN_APP_DEVICE_H__ 1

#include <windows.h>
#include <d3d9.h>

// The app is a singleton around one D3D9 device, and DXContext holds the present
// parameters by pointer, so the device and its parameters live here as globals for
// both the interactive path and the offline renderer to share. Keeping one creation
// path means the adapter choice and the device flags cannot drift apart.

extern IDirect3DDevice9*      pD3DDevice;
extern D3DPRESENT_PARAMETERS  d3dPp;

// bOffline adds the device flags NVENC requires and drops vsync.
// preferredBackBufFormat requests a specific back buffer format (the high-precision
// render path wants D3DFMT_A2R10G10B10); D3DFMT_UNKNOWN means follow the desktop,
// and an unsupported request quietly falls back to it.
bool InitD3d(HWND hwnd, int width, int height, bool bOffline,
             D3DFORMAT preferredBackBufFormat = D3DFMT_UNKNOWN);
void DeinitD3d();

#endif
