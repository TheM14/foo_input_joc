# Verification

Measured results only. Each entry names the command or the log line it came from, so it can
be reproduced. Environment: Windows x64 host, official **foobar2000 1.6.19 x86** portable
installation, official SDK 2026-09-17 pinned to `FOOBAR2000_TARGET_VERSION 80`, MSVC 14.44,
ffmpeg 8.0.

Test material is supplied locally and is **not** part of this repository: the `testdata/` and
`vectors/` files of the upstream renderer project, and — for the binaural measurements — an
HRTF file. Everything except binaural rendering runs without any HRTF; binaural runs take the
file as an argument (`tests/render_harness.cpp --hrtf …`) or use the default location beside
the DLL.

## Component and renderer

| Check | Result |
|---|---|
| Sources compiled in, nothing loaded at run time | log: `core: in-process renderer 0.1.0-m1 (abi 3), component built against abi 3` |
| One artefact, no companion DLL | `dist\*.fb2k-component` holds `foo_input_joc.dll` and `README.md` only |
| Kernel sources untouched | the upstream working tree's file timestamps are unchanged; it is only ever read |
| Both architectures build | `build\Win32\foo_input_joc.dll`, `build\x64\foo_input_joc.dll` |

## Playback in foobar2000 1.6.19 x86

| Case | Log evidence |
|---|---|
| Speaker 7.1, 5 s file | `stream created, 8 output channel(s), layout=7.1` … `frames_in=157 frames_out=157 samples_out=241152` — 157 × 1536 exactly |
| Binaural, SOFA | `stream created, 2 output channel(s)` … `end of stream after 481215 frames` (241152 source + 240063 tail) |
| Binaural, Rosella model | `stream created, 2 output channel(s)` … `end of stream after 481855 frames` |
| Binaural, HRTF path left empty | resolves to `<component directory>\HRTF\binaural.sofa` and produces the same 481215 frames as naming that file explicitly |
| Full 238 s file, binaural SOFA | `eac3 frames queued=7436, bed frames pushed=7436` … `samples_out=11661759`; no ffmpeg process left behind |
| Installed from the `.fb2k-component` package | unpacked into `user-components\foo_input_joc\`, plays with the default-folder HRTF |

## Bitstream recognition

`tests/scan_crosscheck.cpp` compares, frame by frame, the syncframe lengths and the JOC
verdict this component computes against the renderer's own `joc_eac3_frame_bytes()` /
`joc_parse_eac3_frame()`:

```
testdata\gold_forever.eac3  frames=64  plugin_joc=64  kernel_joc=64  len_mismatch=0 verdict_mismatch=0
vectors\valid.eac3          frames=7   plugin_joc=7   kernel_joc=7   len_mismatch=0 verdict_mismatch=0
build\plain_eac3.eac3       frames=64  plugin_joc=0   kernel_joc=0   len_mismatch=0 verdict_mismatch=0
crosscheck: 3 file(s), AGREES WITH CORE
```

Ten corrupt vectors were compared as well: no frame-length disagreement, verdicts agreed on
9 of 10. The one difference is `corrupt_truncated_huffman.eac3`: this component only tests
for the container, while the renderer also parses the payload and reports a truncated
bitstream later.

## Plain E-AC-3 is handed back

Playing a file the component's own encoder produced without JOC:

```
decoder: open "...plain_eac3.eac3" reason=1 bytes=144384 frames=8 with_joc=0
decoder: yielding to the built-in decoder (at least one examined syncframe has no JOC EMDF container)
```

The file then plays through the built-in decoder; no decode log appears for it. Verified both
before and after the renderer was compiled in.

## Output identical to the reference renderer

`tests/render_harness.cpp` drives the component's own engine and writes a WAV in the same
format the reference command-line renderer writes, so the two files can be compared byte for
byte. 30 s of the reference file, speaker 5.1:

| Product | Whole-file SHA-256 |
|---|---|
| reference renderer (`--speaker-layout 5.1 --bed … --duration 30`) | `99a8e3edbd1c047a3c0f547eaf85e56941f882af9e65468fbde0a9f18b6c5b6e` |
| this component's engine, x64 | `99a8e3edbd1c047a3c0f547eaf85e56941f882af9e65468fbde0a9f18b6c5b6e` |
| this component's engine, x86 | `99a8e3edbd1c047a3c0f547eaf85e56941f882af9e65468fbde0a9f18b6c5b6e` |

34,578,500 bytes each. Binaural with a real SOFA file: with the same input frame count the
whole file is identical too (`588a15ce5526977f…baa88`, 12,158,780 bytes), and over the whole
238 s file the total sample count matches exactly (11,661,759 = 11,420,735 program +
241,024 tail) with identical peak (1.144561172) and an identical SHA-256 over the reference
renderer's entire payload.

The one structural difference is the tail: the reference renderer trims trailing samples
below 1e-8 and this component returns the tail in full.

## Gain

Same file, speaker 5.1, rendered with and without attenuation:

| Check | Result |
|---|---|
| Peak | 0.183568597 → 0.092002235, i.e. exactly −6.000000 dB |
| RMS over the whole signal | exactly −6.000000 dB |
| Per-sample, 1,446,912 floats | largest deviation from the ideal scaling is 2.1e-06 relative; the gain is applied in double precision before the DSP, so the remaining difference is float32 rounding |
| +6 dB | same check passes |
| The switch | switch on and −6 dB → log `gain=-6.00 dB`, delivered peak 0.000063; switch off with −6 dB still stored → `gain=0.00 dB`, peak 0.000126; switch on and 0 dB → peak 0.000126 |

## Preferences page

`tests/prefs_layout_check.cpp` builds the page from the component's own dialog resource and
asserts, per control, that it is enabled, lies inside the client area, and is not covered by
another interactive control (static text and group boxes are transparent to the mouse, as
they are for real clicks):

```
dialog client=495x367  non-client=0x0
  WS_CAPTION=no WS_BORDER=no WS_CHILD=yes WS_VISIBLE=yes
content extent=485x354  client=495x367  (everything fits)
controls=26  problems=0
```

The page has no caption of its own — the host draws the frame — and no control is ever
disabled, which is what "the option is there but cannot be clicked" otherwise looks like.

Not verified here: how the page and the `%joc_*%` fields look on screen; that needs a human
in front of the window.

## Container support (mp4 / m4a / mov / mkv / mka / webm)

Verified with the Win32 build, a 5.1 speaker layout, and the component installed in a
portable foobar2000 1.6.19 profile:

- `ffmpeg -i <bare .eac3> -c:a copy` muxed into MP4 and Matroska, then `-map 0:a:0 -c:a copy
  -f eac3` extracted again, is byte-identical to a direct `-t 30 -c:a copy` of the source
  (same SHA-256) — the renderer therefore sees the stored syncframes, not a re-encode.
- The header probe (`tests/container_scan_test.cpp`, no foobar2000 involved) reports, for the
  same material: `joc.mp4 -> mp4 eac3=1 audio#0 codec=ec-3 30.016 s`,
  `joc.mkv -> matroska eac3=1 audio#0 codec=A_EAC3 30.016 s`, `plain_eac3.mp4 -> ec-3`,
  `ac3.mp4 -> ac-3 (declined)`, `aac.mp4 -> mp4a (declined)`.
- End to end, with the component ordered ahead of the container reader in
  Preferences -> Decoding: `open()` is called, the track is found, the JOC verdict is positive,
  the file is claimed, and playback reaches `end of stream`.
- Negative case end to end: an MP4 holding E-AC-3 without JOC yields with
  `E-AC-3 track 0 carries no JOC`, and the built-in decoder plays it.
- Bare `.eac3` / `.ec3` handling is unchanged.

## Bed decode and gain

* The 5.1 bed is decoded with -drc_scale 0 -target_level 0, so it is taken as stored and the
  decoder does not apply the stream's dynrng or target-level metadata. The resulting bed is
  byte-identical to the one the reference renderer uses (same SHA-256).
* The master gain is applied once, by the JOC kernel, on every output path. Measured at -6 dB
  against 0 dB: speaker 5.1 and SOFA binaural give 0.50119 in peak and in per-channel RMS,
  Rosella binaural gives 0.50119 as well, and a 0 dB render is unaffected by the renderer's
  output gain.