#include "frame_grabber.h"
#include <d3dx9.h>

namespace offline {

FrameGrabber::FrameGrabber()
    : m_device(NULL), m_staging(NULL), m_width(0), m_height(0),
      m_format(D3DFMT_UNKNOWN), m_locked(false)
{
}

FrameGrabber::~FrameGrabber()
{
    Shutdown();
}

bool FrameGrabber::Init(IDirect3DDevice9* device, int width, int height)
{
    Shutdown();

    if (!device || width <= 0 || height <= 0)
        return false;

    // Take the format from the render target rather than assuming: the
    // high-precision path presents A2R10G10B10 where the faithful path is
    // X8R8G8B8, and GetRenderTargetData requires the two to match exactly.
    IDirect3DSurface9* rt = NULL;
    if (FAILED(device->GetRenderTarget(0, &rt)) || !rt)
        return false;

    D3DSURFACE_DESC desc;
    const HRESULT hrDesc = rt->GetDesc(&desc);
    rt->Release();
    if (FAILED(hrDesc))
        return false;

    if ((int)desc.Width != width || (int)desc.Height != height)
        return false;

    if (FAILED(device->CreateOffscreenPlainSurface(
            width, height, desc.Format, D3DPOOL_SYSTEMMEM, &m_staging, NULL)))
        return false;

    m_device = device;
    m_device->AddRef();
    m_width  = width;
    m_height = height;
    m_format = desc.Format;
    return true;
}

void FrameGrabber::Shutdown()
{
    if (m_locked)
        Unlock();

    if (m_staging) { m_staging->Release(); m_staging = NULL; }
    if (m_device)  { m_device->Release();  m_device  = NULL; }

    m_width = m_height = 0;
    m_format = D3DFMT_UNKNOWN;
}

bool FrameGrabber::Grab(const void** bits, int* pitchBytes)
{
    if (!m_device || !m_staging || m_locked)
        return false;

    IDirect3DSurface9* rt = NULL;
    if (FAILED(m_device->GetRenderTarget(0, &rt)) || !rt)
        return false;

    const HRESULT hr = m_device->GetRenderTargetData(rt, m_staging);
    rt->Release();
    if (FAILED(hr))
        return false;

    D3DLOCKED_RECT lr;
    if (FAILED(m_staging->LockRect(&lr, NULL, D3DLOCK_READONLY)))
        return false;

    m_locked = true;
    if (bits)       *bits = lr.pBits;
    if (pitchBytes) *pitchBytes = lr.Pitch;
    return true;
}

void FrameGrabber::Unlock()
{
    if (m_locked && m_staging)
        m_staging->UnlockRect();
    m_locked = false;
}

bool FrameGrabber::SaveLastFrame(const wchar_t* path)
{
    if (!m_staging || m_locked)
        return false;
    return SUCCEEDED(D3DXSaveSurfaceToFileW(path, D3DXIFF_PNG, m_staging, NULL, NULL));
}

} // namespace offline
