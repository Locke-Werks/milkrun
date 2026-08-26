#ifndef __MILKRUN_FRAME_GRABBER_H__
#define __MILKRUN_FRAME_GRABBER_H__ 1

#include <windows.h>
#include <d3d9.h>

namespace offline {

// Pulls finished frames off the GPU.
//
// The offline path never calls Present, so after PluginRender returns, the frame
// is sitting in the back buffer (the swap effect is D3DSWAPEFFECT_COPY, so its
// contents persist). This copies that into a system-memory surface and hands out
// a pointer to the pixels.
class FrameGrabber
{
public:
    FrameGrabber();
    ~FrameGrabber();

    bool Init(IDirect3DDevice9* device, int width, int height);
    void Shutdown();

    // Copies the current render target into system memory and locks it.
    // Every successful Grab must be paired with an Unlock.
    bool Grab(const void** bits, int* pitchBytes);
    void Unlock();

    D3DFORMAT Format() const { return m_format; }
    int  Width()  const { return m_width; }
    int  Height() const { return m_height; }

    // Writes the most recently grabbed frame out as a PNG. For eyeballing output
    // and for the live-versus-offline parity check.
    bool SaveLastFrame(const wchar_t* path);

private:
    IDirect3DDevice9*   m_device;
    IDirect3DSurface9*  m_staging;
    int                 m_width;
    int                 m_height;
    D3DFORMAT           m_format;
    bool                m_locked;
};

} // namespace offline

#endif
