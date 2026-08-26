<div align="center">

<img src="assets/milkrun.ico" width="96" alt="Milk Run">

# Milk Run

**Renders a MilkDrop preset to video, in sync with a song, start to finish, nothing extra.**

[![build](https://img.shields.io/github/actions/workflow/status/Locke-Werks/milkrun/build.yml?branch=main&style=flat-square&color=d6262a)](https://github.com/Locke-Werks/milkrun/actions/workflows/build.yml)
[![license](https://img.shields.io/badge/license-BSD--3--Clause-d6262a?style=flat-square)](LICENSE)
[![platform](https://img.shields.io/badge/platform-Windows%2010%2F11-d6262a?style=flat-square)](#requirements)

</div>

---

MilkDrop is a live visualizer. It reads whatever your system is playing, animates against
the wall clock, and draws to a window. Capturing that means screen recording it in real
time, which drops frames, bakes in whatever else the machine was doing, and cannot hit an
exact resolution or frame rate.

Milk Run renders the same visualizer offline instead. Pick a preset, a song, a frame rate
and a size; it produces one H.265 Main10 file whose visuals are driven by that song's
actual samples, whose duration equals the song's duration, and which carries no overlays,
no lead-in and no tail.

Because nothing is tied to the wall clock, the output is deterministic: the same job
rendered twice is byte-identical, whether the machine managed 200 frames per second or 5.

## Requirements

- Windows 10 or 11
- A GPU that can encode HEVC. NVIDIA goes through NVENC; AMD and Intel go through the
  encoder Media Foundation exposes.
- Preset files (`.milk`) and their textures. None ship here; point it at an existing
  MilkDrop or Winamp preset folder.

## Rendering

```
MilkRun.exe --render --preset "presets\foo.milk" --audio "song.flac" --out out.mp4
```

| Option | |
| --- | --- |
| `--size WxH` | output dimensions, default 1920x1080 |
| `--fps N` | frame rate; accepts `60`, `24000/1001` or `29.97`. Default 60 |
| `--start S` `--duration S` | render a section, for auditioning a preset against a chorus |
| `--precision faithful\|high` | 8-bit, matching the live look, or a float canvas into a 10-bit back buffer |
| `--pq2020` | HDR10 output. Needs `--precision high` |
| `--encoder auto\|nvenc\|mf` | which HEVC encoder to use |
| `--quality N` | constant-quality level, lower is better |
| `--bitrate K` | target bitrate instead of constant quality |

`--probe-audio <file>` prints the peak sample in each frame's audio window, which is the
quickest way to confirm a file decodes and that a transient lands where you expect.

## In the app

| | |
| --- | --- |
| `Ctrl+R` | Render the running preset to a file |
| `Ctrl+,` | Settings |
| `F1` | MilkDrop's own help, and the rest of its shortcuts |

Settings covers the preset folder, which GPU to render on, the frame cap, canvas
colour depth and size, preset timing and hard cuts, and the on-screen readouts.
The adapter and the canvas settings need a device rebuild, so those apply the
next time Milk Run starts; everything else takes effect immediately.

## Output

Always H.265 Main10 (`hvc1`, `yuv420p10le`) in MP4, with the song muxed in as AAC. The
frame count is `ceil(duration * fps)` exactly, so the video runs for as long as the song
and not a frame longer.

`--precision high` renders the internal canvas at 16-bit float into a 10-bit back buffer.
This matters more than it sounds: MilkDrop blends additively and routinely pushes values
past 1.0, which an 8-bit canvas clips to white. `--pq2020` builds on that, converting to
BT.2020 primaries with the SMPTE ST 2084 curve so those overbrights become real highlights
above diffuse white. That is a genuine pixel conversion, not a stream tag.

## Building

```
git clone https://github.com/Locke-Werks/milkrun
cd milkrun
./scripts/fetch-deps.ps1
```

Then open `code/MilkDrop3.sln` in Visual Studio 2022 and build `Release|Win32`.

The fetch step pulls D3DX9 out of Microsoft's `Microsoft.DXSDK.D3DX` NuGet package. That
is the supported replacement for installing the discontinued DirectX SDK (June 2010), and
it sidesteps that installer's S1023 failure on machines carrying a newer VC++ runtime. It
is fetched rather than committed because Microsoft's terms permit shipping the
redistributable DLLs inside an application but not republishing the headers and import
library.

NVENC headers are vendored under `code/third_party/nvenc`, which their licence does allow.
The encoder itself is loaded at run time, so the binary still starts on a machine with no
NVIDIA GPU.

x86 only, and not by preference: the NS-EEL expression evaluator that runs preset equations
JITs x86 machine code and has no x64 path.

## Credits

Forked from [MilkDrop 3](https://github.com/milkdrop2077/MilkDrop3) by milkdrop2077, which
is itself built on BeatDrop by Maxim Volskiy, which descends from the original MilkDrop by
Ryan Geiss. The visualizer, the preset format and the years of work behind them are theirs;
Milk Run only adds the offline renderer.

The upstream feature and keyboard reference is preserved at
[docs/milkdrop3.md](docs/milkdrop3.md).

## License

BSD 3-Clause, unchanged from upstream. See [LICENSE](LICENSE).
