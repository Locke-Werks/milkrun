#ifndef __MILKRUN_RENDER_CONFIG_H__
#define __MILKRUN_RENDER_CONFIG_H__ 1

#include <string>

namespace offline {

// How much precision the render pipeline carries.
enum class Precision
{
    // 8-bit internal canvas and back buffer, exactly as the visualizer runs live.
    // The encoder still emits a Main10 stream; it widens the 8-bit input itself.
    Faithful,

    // 16-bit float canvas into a 10-bit back buffer. Genuinely 10-bit end to end
    // and far less banding on gradient-heavy presets, but MilkDrop blends into the
    // canvas continuously, so higher canvas precision can look subtly unlike live.
    High
};

enum class EncoderBackend
{
    Auto,               // NVENC if present, otherwise Media Foundation
    Nvenc,
    MediaFoundation
};

struct RenderJobConfig
{
    std::wstring presetPath;    // full path to a .milk
    std::wstring audioPath;     // the song driving the visualization
    std::wstring outputPath;    // .mp4 to write

    int width  = 1920;
    int height = 1080;

    // Kept as a rational so 23.976 and 29.97 are exact rather than rounded.
    int fpsNum = 60;
    int fpsDen = 1;

    Precision      precision = Precision::Faithful;

    // HDR10 output: BT.2020 primaries with the SMPTE ST 2084 (PQ) transfer curve.
    // Only meaningful alongside Precision::High, because the conversion needs the
    // float canvas: MilkDrop's additive blending pushes values well above 1.0, and
    // those are exactly what become real highlights above diffuse white.
    //
    // This is a genuine pixel conversion, not just stream tagging. Tagging alone
    // would make the file render far too dark on an HDR display.
    bool pq2020 = false;

    // Where SDR diffuse white lands on the PQ curve, in nits. 203 is the BT.2408
    // reference level for mapping SDR content into HDR.
    float diffuseWhiteNits = 203.0f;

    // Peak the PQ encode is allowed to reach, and what gets signalled as the
    // mastering display maximum.
    float peakNits = 1000.0f;
    EncoderBackend backend   = EncoderBackend::Auto;

    int bitrateKbps = 0;   // 0 means use quality instead of a target bitrate
    int quality     = 24;  // constant-quality level; lower is better

    // Render a section rather than the whole song. Useful for auditioning a preset
    // against a chorus without committing to a full pass. The visual clock still
    // starts at zero, so the preset begins fresh; only the audio is offset.
    double startSeconds    = 0.0;
    double durationSeconds = 0.0;   // 0 means run to the end of the song

    // Render exactly this many frames instead of deriving the count from the audio
    // duration. Only for smoke tests; 0 means "as long as the song".
    long long frameCountOverride = 0;

    // Write frames out as PNGs alongside encoding. Empty means don't. Useful for
    // eyeballing output and for the live-versus-offline parity check.
    std::wstring dumpFramesDir;
    int dumpFrameStride = 1;   // dump every Nth frame

    // Fixed so that re-rendering the same job produces an identical file. MilkDrop
    // calls rand() per frame to feed m_rand_frame into the pixel shaders.
    unsigned randomSeed = 1;
};

} // namespace offline

#endif
