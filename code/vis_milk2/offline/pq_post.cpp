#include "pq_post.h"
#include <d3dx9.h>

namespace offline {

namespace {

struct QuadVertex
{
    float x, y, z, rhw;
    float u, v;
};

const DWORD kQuadFvf = D3DFVF_XYZRHW | D3DFVF_TEX1;

// Written against ps_2_0 so it runs anywhere the rest of MilkDrop does.
//
// c0 = (diffuseWhiteNits, peakNits, unused, unused)
const char kPqShader[] =
"sampler2D src : register(s0);                                              \n"
"float4 g_params : register(c0);                                            \n"
"                                                                           \n"
"float3 LinearToPq(float3 L)                                                \n"
"{                                                                          \n"
"    // SMPTE ST 2084 inverse EOTF. L is absolute luminance normalised so    \n"
"    // that 1.0 means 10000 nits, which is what PQ code values encode.      \n"
"    const float m1 = 0.1593017578125;                                      \n"
"    const float m2 = 78.84375;                                             \n"
"    const float c1 = 0.8359375;                                            \n"
"    const float c2 = 18.8515625;                                           \n"
"    const float c3 = 18.6875;                                              \n"
"    float3 Lm = pow(max(L, 0.0), m1);                                      \n"
"    return pow((c1 + c2 * Lm) / (1.0 + c3 * Lm), m2);                      \n"
"}                                                                          \n"
"                                                                           \n"
"float4 main(float2 uv : TEXCOORD0) : COLOR                                 \n"
"{                                                                          \n"
"    float3 c = max(tex2D(src, uv).rgb, 0.0);                               \n"
"                                                                           \n"
"    // MilkDrop's canvas holds display-referred sRGB-ish values, so undo    \n"
"    // that curve to get back to linear light before touching primaries.   \n"
"    // Values above 1.0 survive here and become the highlights.            \n"
"    float3 lin = pow(c, 2.2);                                              \n"
"                                                                           \n"
"    // Rec.709 primaries to Rec.2020, in linear light.                     \n"
"    float3 wide;                                                           \n"
"    wide.r = dot(lin, float3(0.6274, 0.3293, 0.0433));                     \n"
"    wide.g = dot(lin, float3(0.0691, 0.9195, 0.0114));                     \n"
"    wide.b = dot(lin, float3(0.0164, 0.0880, 0.8956));                     \n"
"                                                                           \n"
"    // SDR 1.0 sits at diffuse white; anything brighter runs up to peak.   \n"
"    float3 nits = min(max(wide, 0.0) * g_params.x, g_params.y);            \n"
"                                                                           \n"
"    return float4(LinearToPq(nits / 10000.0), 1.0);                        \n"
"}                                                                          \n";

} // namespace

PqPostProcess::PqPostProcess()
    : m_device(NULL), m_texture(NULL), m_surface(NULL), m_shader(NULL),
      m_width(0), m_height(0), m_diffuseWhiteNits(203.0f), m_peakNits(1000.0f),
      m_ready(false)
{
}

PqPostProcess::~PqPostProcess()
{
    Shutdown();
}

bool PqPostProcess::Init(IDirect3DDevice9* device, int width, int height,
                         float diffuseWhiteNits, float peakNits, std::wstring& error)
{
    Shutdown();

    if (!device || width <= 0 || height <= 0)
    {
        error = L"Invalid parameters for the HDR conversion pass.";
        return false;
    }

    m_device = device;
    m_device->AddRef();
    m_width  = width;
    m_height = height;
    m_diffuseWhiteNits = (diffuseWhiteNits > 0.0f) ? diffuseWhiteNits : 203.0f;
    m_peakNits         = (peakNits > 0.0f) ? peakNits : 1000.0f;

    // Half float, so the additive overbrights MilkDrop produces are still there to
    // map into highlights. An integer intermediate would clip them to white first.
    if (FAILED(m_device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET,
                                       D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT,
                                       &m_texture, NULL)))
    {
        error = L"This GPU cannot provide the floating point buffer HDR output needs.";
        Shutdown();
        return false;
    }

    if (FAILED(m_texture->GetSurfaceLevel(0, &m_surface)))
    {
        error = L"Could not access the HDR intermediate buffer.";
        Shutdown();
        return false;
    }

    LPD3DXBUFFER code = NULL;
    LPD3DXBUFFER errors = NULL;
    HRESULT hr = D3DXCompileShader(kPqShader, (UINT)strlen(kPqShader),
                                   NULL, NULL, "main", "ps_2_0",
                                   0, &code, &errors, NULL);
    if (FAILED(hr) || !code)
    {
        error = L"The HDR conversion shader failed to compile.";
        if (errors) errors->Release();
        if (code) code->Release();
        Shutdown();
        return false;
    }
    if (errors) errors->Release();

    hr = m_device->CreatePixelShader((DWORD*)code->GetBufferPointer(), &m_shader);
    code->Release();

    if (FAILED(hr))
    {
        error = L"The HDR conversion shader could not be created.";
        Shutdown();
        return false;
    }

    m_ready = true;
    return true;
}

void PqPostProcess::Shutdown()
{
    if (m_shader)  { m_shader->Release();  m_shader = NULL; }
    if (m_surface) { m_surface->Release(); m_surface = NULL; }
    if (m_texture) { m_texture->Release(); m_texture = NULL; }
    if (m_device)  { m_device->Release();  m_device = NULL; }
    m_ready = false;
}

bool PqPostProcess::Resolve(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer)
{
    if (!m_ready || !device || !backBuffer)
        return false;

    if (FAILED(device->SetRenderTarget(0, backBuffer)))
        return false;

    // D3D9 samples texels at their corners, so a full-screen blit needs the half
    // texel shift or the whole image lands half a pixel off.
    const float w = (float)m_width;
    const float h = (float)m_height;
    QuadVertex v[4];
    v[0].x = -0.5f;     v[0].y = -0.5f;     v[0].u = 0.0f; v[0].v = 0.0f;
    v[1].x = w - 0.5f;  v[1].y = -0.5f;     v[1].u = 1.0f; v[1].v = 0.0f;
    v[2].x = -0.5f;     v[2].y = h - 0.5f;  v[2].u = 0.0f; v[2].v = 1.0f;
    v[3].x = w - 0.5f;  v[3].y = h - 0.5f;  v[3].u = 1.0f; v[3].v = 1.0f;
    for (int i = 0; i < 4; i++) { v[i].z = 0.0f; v[i].rhw = 1.0f; }

    device->SetVertexShader(NULL);
    device->SetFVF(kQuadFvf);
    device->SetPixelShader(m_shader);
    device->SetTexture(0, m_texture);

    // Point sampling: this is a one to one blit, so anything else would only blur.
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);

    const float params[4] = { m_diffuseWhiteNits, m_peakNits, 0.0f, 0.0f };
    device->SetPixelShaderConstantF(0, params, 1);

    const HRESULT hr = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(QuadVertex));

    device->SetPixelShader(NULL);
    device->SetTexture(0, NULL);

    return SUCCEEDED(hr);
}

} // namespace offline
