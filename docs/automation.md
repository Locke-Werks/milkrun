# Driving Milk Run from a script

Everything the app does interactively is reachable from the command line, so a
render needs no window and no keyboard. This is the reference for doing it
programmatically.

Every behaviour below was checked against the shipping binary. Where something
is surprising, the reason is given rather than left to be rediscovered.

## Where the binary is

An installed copy lives at `C:\Program Files\Milk Run\MilkRun.exe`. A build tree
puts it in `code\vis_milk2\Release\MilkRun.exe`.

Run it from its own directory, or with that directory as the working directory.
It resolves `plugins\Milkdrop2\data\*.fx` relative to the executable, and those
eight shader files are mandatory: without them it stops with a message box, which
in an unattended run means it hangs rather than exits.

## The shortest useful invocation

```powershell
& "C:\Program Files\Milk Run\MilkRun.exe" --render `
    --preset "C:\presets\Geiss - Cauldron.milk" `
    --audio  "C:\music\track.flac" `
    --out    "C:\out\track.mp4"
```

That renders the whole song at 1920x1080, 60 fps, H.265 Main10, with the song
muxed in as AAC. The video runs for exactly as long as the audio.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Success |
| 1 | The render failed. The reason is on stdout |
| 2 | The arguments were wrong. Nothing was attempted |

Check the code, not the output text. Codes 1 and 2 are worth telling apart: 2
means a script bug, 1 means the machine or the inputs.

## Reading the output

`MilkRun.exe` is a windows-subsystem binary, so it owns no console. It writes to
whatever stdout it inherits, which means piping and redirection both work:

```powershell
$out = & "C:\Program Files\Milk Run\MilkRun.exe" --render ... 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) { throw $out }
```

Launched from a console with no redirection it attaches to that console instead.
Launched from a GUI with neither, the writes go nowhere and the render still
runs. Progress is reported every 10% when piped and redrawn on one line when
attached to a console, so a log file stays readable.

## Options

| Option | Default | |
| --- | --- | --- |
| `--render` | | Required to render at all |
| `--preset <file.milk>` | | Required |
| `--audio <file>` | | Required unless `--frames` is given |
| `--out <file.mp4>` | | Required unless `--dump-frames` is given |
| `--size WxH` | `1920x1080` | |
| `--fps N` | `60` | Accepts `60`, `24000/1001`, `29.97` |
| `--start S` | `0` | Begin S seconds into the song |
| `--duration S` | rest of song | Render only S seconds |
| `--quality N` | `24` | Constant quality, lower is better |
| `--bitrate K` | | Target kbps instead of constant quality |
| `--precision faithful\|high` | `faithful` | |
| `--pq2020` | off | HDR10. Requires `--precision high` |
| `--encoder auto\|nvenc\|mf` | `auto` | |
| `--seed N` | `1` | |
| `--play` | off | Open the result in the built-in player when done |
| `--frames N` | | Render N frames instead of following the song |
| `--dump-frames <dir>` | | Also write frames out as PNGs |
| `--dump-stride N` | `1` | Dump every Nth frame |
| `--probe-audio <file>` | | Diagnostic, see below |
| `--help` | | |

## Things that will catch a script out

**Quote every path.** Preset filenames are full of spaces, ampersands and
brackets: `Rovastar & Krash - Cerebral Demons (Beat Pulse Mix).milk` is typical.
In PowerShell, passing an unquoted path through `Start-Process -ArgumentList`
splits it and the run fails with exit 2.

**`--play` blocks.** It opens a player window and does not return until that
window closes. Never use it unattended.

**Rendering needs a GPU that can encode HEVC.** CI runners generally have none,
so a render cannot be smoke-tested there. `--probe-audio` can, because it only
touches the audio front end.

**`--frames` ignores the song length**, so it is for smoke tests, not for
producing a clip. Use `--start` and `--duration` for a real excerpt.

**A render is a whole session.** It creates its own device and drives the same
plugin the interactive app does, so do not run one in the same process as a live
window. Separate processes are fine; run as many as the GPU will take.

## Verifying the result

The output is H.265 Main10 in MP4, tagged `hvc1`, `yuv420p10le`, with the frame
count equal to `ceil(songDuration * fps)`. Nothing is padded onto either end.

```powershell
ffprobe -v error -show_entries stream=codec_name,profile,pix_fmt,width,height,r_frame_rate,nb_frames `
        -of default=nw=1 out.mp4
```

With `--pq2020`, also expect `color_primaries=bt2020`,
`color_transfer=smpte2084`, `color_space=bt2020nc`.

Renders are deterministic: the same job twice is byte identical, whatever frame
rate the machine managed. Two runs can be compared with
`ffmpeg -i <file> -f framemd5 -`. If they differ, something is wrong, not merely
different.

## Checking a song before committing to a render

`--probe-audio` prints the peak sample in each frame's audio window. It is the
quickest way to confirm a file decodes at all, that it is not silent, and that a
transient lands where it should.

```powershell
& MilkRun.exe --probe-audio "track.wav" --fps 60 --from 0 --to 10
```

```
  duration 105.854042s, 6352 frames at 60.0000 fps

  frame    window start (s)    peak L   peak R
      0          0.000000        75       76
      1          0.016667        59       60
```

Peaks are 0 to 127. A file that decodes but reports all zeros is silent, and a
render of it produces a still-looking video rather than an error.

Most formats go through Media Foundation, so mp3, aac, m4a, flac and ordinary
wav all work. WAV files are read directly instead, which is the only way to
handle the 64-bit float exports DAWs produce; Media Foundation refuses PCM wider
than 32-bit float.

## Batch rendering

```powershell
$exe = "C:\Program Files\Milk Run\MilkRun.exe"
Get-ChildItem "C:\presets\*.milk" | ForEach-Object {
    $out = Join-Path "C:\out" ($_.BaseName + ".mp4")

    $log = & $exe --render `
        --preset $_.FullName `
        --audio  "C:\music\track.flac" `
        --start 30 --duration 20 `
        --size 1920x1080 --fps 60 `
        --out $out 2>&1 | Out-String

    if ($LASTEXITCODE -ne 0) {
        Write-Warning "$($_.Name): $log"
        return              # the failed output is deleted, so nothing partial is left
    }
    "$($_.Name) -> $out"
}
```

A failed or cancelled render deletes its own output rather than leaving a partial
file, so the presence of the file is a reliable success signal on its own.
