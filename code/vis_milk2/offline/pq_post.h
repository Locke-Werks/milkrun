#ifndef __MILKRUN_PQ_POST_H__
#define __MILKRUN_PQ_POST_H__ 1

#include <windows.h>
#include <d3d9.h>
#include <string>

namespace offline {

// Converts a finished frame from MilkDrop's SDR output into HDR10: BT.2020
// primaries with the SMPTE ST 2084 (PQ) transfer curve.
//
// This has to be a real pixel conversion. Tagging the stream as PQ without
// transforming the pixels produces a file that renders far too dark on an HDR
// display, because PQ code values are absolute luminance rather than relative.
//
// It cannot live inside MilkDrop's composite shader, because presets are free to
// supply their own comp_ps and would overwrite it. So the composite is redirected
// into a float intermediate, and this runs afterwards as a full-screen pass into
// the real back buffer.
//
// The payoff is specific to this content: MilkDrop blends additively and routinely
// pushes values well past 1.0. On a float canvas those survive instead of clipping,
// and become genuine highlights above diffuse white.
class PqPostProcess
{
public:
    PqPostProcess();
    ~PqPostProcess();

    // diffuseWhiteNits is where SDR 1.0 lands (BT.2408 reference is 203).
    // peakNits caps the result and is what gets signalled as the mastering peak.
    bool Init(IDirect3DDevice9* device, int width, int height,
              float diffuseWhiteNits, float peakNits, std::wstring& error);
    void Shutdown();

    bool IsReady() const { return m_ready; }

    // The surface MilkDrop should composite into instead of the back buffer.
    // Borrowed, not owned; do not release it.
    IDirect3DSurface9* CompositeTarget() const { return m_surface; }

    // Draws the intermediate into the given back buffer, applying the transform.
    bool Resolve(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer);

private:
    IDirect3DDevice9*      m_device;
    IDirect3DTexture9*     m_texture;
    IDirect3DSurface9*     m_surface;
    IDirect3DPixelShader9* m_shader;

    int   m_width;
    int   m_height;
    float m_diffuseWhiteNits;
    float m_peakNits;
    bool  m_ready;
};

} // namespace offline

#endif
