# foo_input_joc — foobar2000 input component for E-AC-3 JOC

[简体中文](README.md) · [Install](#install) · [Settings](#settings) · [Building](#building) · [Known limitations](#known-limitations)

A foobar2000 input component for **E-AC-3 JOC (Dolby Atmos)** files: the JOC object and OAMD
metadata is taken from the E-AC-3 syncframes and paired with the core PCM that ffmpeg decodes
(5.1 or 7 channels, following $joc\_dmx\_config\_idx$), then rendered in real time to headphones
(HRTF) or to a speaker layout from 2.0 up to 9.1.6. The
rendering core is compiled into the component, so there is nothing to install beside
`foo_input_joc.dll`.

Input is a bare `.eac3` / `.ec3` stream, or an E-AC-3 JOC track inside a container (`.mp4`
`.m4a` `.m4b` `.m4p` `.m4r` `.mov` `.mkv` `.mka` `.webm`). A file without JOC is not claimed:
plain AC-3 / E-AC-3, a container whose audio track is AAC, and transport streams are all handed
back to foobar2000 and play through the decoder it would have used anyway.

## Requirements

* foobar2000 1.6 (32-bit) or 2.x (32-bit and 64-bit).
* An `ffmpeg` executable, found on `PATH` by default; the preferences page can name one.
* For binaural output, an HRTF data file. **This repository does not ship one** — see
  [HRTF data](#hrtf-data).

## Install

Download the package for your architecture from [Releases](../../releases) — a pushed `v*` tag
makes CI attach both — or build it yourself as described under [Building](#building) and take the
result from `dist\`; either way the file is `foo_input_joc-<version>-<arch>.fb2k-component`. Drop
it onto foobar2000, or use Preferences → Components → Install, and restart. Take `-x86` for
foobar2000 1.6 and 2.x 32-bit, `-x64` for 2.x 64-bit.

To install by hand, copy `foo_input_joc.dll` into the per-component subdirectory of the
`user-components` folder the running version reads:

| foobar2000 | Folder |
|---|---|
| 1.6 | `<profile>\user-components\foo_input_joc\` |
| 2.x | `<app>\user-components\foo_input_joc\` (also in portable mode) |

The subdirectory is required — a DLL lying directly in `user-components\` is not scanned — and a
component placed in the folder the running version does not read, or built for the other
architecture, is **ignored without a message**.

`tools/deploy.ps1 -TestBed <portable foobar2000>` is the scripted form of the manual steps, and
`tools/run.ps1 -TestBed <path> -Play <file>` plays a file unattended and prints the log. Let
foobar2000 exit through `/exit`: a force-killed instance leaves a `<profile>\running` marker
behind, and the next start then refuses to load any user component.

### Containers need one look at the decoder list

foobar2000 asks the decoders in the order shown in Preferences → **Decoding**, and the built-in
container readers are in that list. When one of them is offered an MP4 or Matroska file first, it
takes the file and the JOC objects are lost — such a file then plays as plain E-AC-3.

So move **JOC decoder (E-AC-3 JOC)** above **foobar2000 MP4 Demuxer** and **foobar2000
Matroska/WebM Reader** in that list. Bare `.eac3` / `.ec3` files do not depend on the order.

## Settings

Preferences → Tools → **JOC decoder**:

* **Output** — binaural, or a speaker layout from 2.0 to 9.1.6 (see
  [Speaker layouts and channel order](#speaker-layouts-and-channel-order));
* **Binaural mode** (near / mid / far) and the room **tail** in seconds;
* **HRTF source** — a **SOFA** file or a **Rosella** `.personalized_headphone` model. An empty
  path means the default location, `<component directory>\HRTF\binaural.sofa` or
  `binaural.personalized_headphone`;
* **Gain** — a switch and a value in dB. Binaural rendering can exceed full scale on material
  that does not clip in the core mix, so attenuation belongs here;
* **Force 24-bit output (int24)** — off by default, which delivers float32. When on, every sample
  is quantised with the core's own int24 rule (clip to ±1, ×8388607, truncate) before it is handed
  over, the samples that had to be clipped are counted in the log, and the component reports the
  file as 24-bit fixed point. The renderer itself stays float32: this is the precision the decoder
  delivers, not the output device's bit depth, which is set in Preferences → Output. **Reload a
  file's information after flipping this switch** (Properties → *Reload info*, or remove the file
  from the Media Library and add it again): foobar2000 caches technical information per file, and
  until it is reloaded Properties and the Converter still go by the old 32-bit floating-point
  answer, warning that the encoder does not take floating-point input.
* the **ffmpeg** executable to use.

No control on the page is disabled, and the status line states what is in effect.

### Speaker layouts and channel order

| Layout | Channels | Channel mask | Delivered order (declared speaker positions) |
|---|---|---|---|
| 2.0 | 2 | `0x3` | FL FR |
| 3.1 | 4 | `0xF` | FL FR FC LFE |
| 5.1 | 6 | `0x3F` | FL FR FC LFE BL BR |
| 7.1 | 8 | `0x63F` | FL FR FC LFE BL BR SL SR |
| 5.1.2 | 8 | `0x560F` | FL FR FC LFE SL SR TFL TFR |
| 5.1.4 | 10 | `0x2D60F` | FL FR FC LFE SL SR TFL TFR TBL TBR |
| 7.1.2 | 10 | `0x563F` | FL FR FC LFE BL BR SL SR TFL TFR |
| 7.1.4 | 12 | `0x2D63F` | FL FR FC LFE BL BR SL SR TFL TFR TBL TBR |
| 9.1.4 | 14 | `0x2D6FF` | FL FR FC LFE BL BR FCL FCR SL SR TFL TFR TBL TBR |
| 9.1.6 | 16 | `0x3F6FF` | FL FR FC LFE BL BR FCL FCR SL SR TFL TFC TFR TBL TBC TBR |

The renderer emits the reference implementation's channel order (for 9.1.4: L R C LFE Lrs Rrs Ls Rs
Ltf Rtf Ltr Rtr Lw Rw), while foobar2000 defines the interleaved order as the ascending order of the
channel mask's speaker bits. The two agree for 2.0 through 7.1.4; 9.1.4 and 9.1.6 carry the wide
pair last, so the channels are permuted into mask order before delivery (the log says `channels
permuted into mask order`). The table above is that order; Properties shows the matching
`WAVEFORMATEXTENSIBLE_CHANNEL_MASK`, and the Converter writes files in the same order.

The mask follows ffmpeg's definition of the same layout name (`5.1` is BL/BR, `5.1.2` and up are
SL/SR, the wide pair is FCL/FCR), so a layout name means the same thing in the component, in a
converted file and in other tools. **9.1.6's top-middle pair (Ltm/Rtm) has no WAVE position** — ffmpeg
places it at TSL/TSR, beyond a 32-bit mask — so it is delivered at the two centre positions the mask
has left, TFC and TBC: the mask still has exactly one bit per channel and the order is the one in the
table, at the cost of those two channels being labelled top front centre / top back centre in
foobar2000's channel lists.

**Layouts other than 2.0 / 5.1 / 7.1 need ASIO (or an exclusive output that accepts the
layout).** foobar2000's shared and exclusive outputs follow the Windows speaker configuration, of
which those are the only multichannel ones; ASIO has no such limit and maps the channels in the
order the component delivers them (assign them by name in fb2k's ASIO channel mapping). With such a
layout on a system-configured output the core downmixes the extra channels — audible, but those
channels are not heard; the Converter is not bound by the output device and writes 14- or
16-channel files as configured. The preferences status line says "needs ASIO output" when such a
layout is selected.

### HRTF data

A SOFA measurement set or a personalised headphone model is supplied by whoever runs the
component, and is listed in `.gitignore` so it cannot be committed by accident. Speaker layouts
need none. Binaural rendering without an HRTF fails with a message naming the file it looked for.

## Building

```powershell
pwsh -File tools/setup_sdk.ps1          # official SDK into SDK/, pinned to target 1.5/1.6
pwsh -File tools/build.ps1              # Win32 -> build\Win32\foo_input_joc.dll
pwsh -File tools/build.ps1 -Platform x64
pwsh -File tools/package.ps1            # both, packaged into dist\*.fb2k-component
```

The configuration is fixed at `Release-Static` (static CRT, `/MT`), and `/fp:precise` is what the
byte-for-byte acceptance rests on, so it must not be changed. `foo_input_joc.vcxproj` builds
`kernel\joc_kernel.vcxproj` first through a project reference; the rendering core sources in
`kernel/` are compiled with `JOC_STATIC` / `EJOC_STATIC`, so their entry points are neither
imported nor exported. `tests\` holds the offline tools (bitstream self-test and cross-check,
render comparison, speaker-layout table and channel-order check, preferences-page layout check,
container-probe check) and `tools\` the build and test-bed scripts. Settings also read `JOC_*`
environment overrides for one run (development only); the list and what each one does is in
`src\settings.cpp`.

To diagnose a problem, read `joc_decoder.log` beside the DLL: the component writes its own
version, the core version and the log path there at start-up. The log is capped by **bytes**: a
file that reaches 16 MiB is rolled to `joc_decoder.log.1` beside it, replacing the previous roll
rather than accumulating, so the two together never exceed 32 MiB and the newest lines are always
in `joc_decoder.log`. The version banner is written again after a roll. A Media Library scan logs
every container in the library, which is where the volume comes from.

## Known limitations

* ADM BWF output is not implemented.
* Speaker layouts go up to 9.1.6 (16 channels), which is the renderer's own limit — there is no
  22.2. Layouts other than 2.0 / 5.1 / 7.1 need ASIO (or an exclusive output that accepts the
  layout); see [Speaker layouts and channel order](#speaker-layouts-and-channel-order).
* 9.1.6's top-middle pair has no WAVE channel position and is delivered at TFC/TBC (same section).
* All five $joc\_dmx\_config\_idx$ configurations are supported: the bed is paired per audio
  frame, and its channel count follows the configuration (6 channels for 0/3, 8 for 1/2/4), with
  `-ac 6` passed to ffmpeg only for the 6-channel ones. An AC-3 core plus a dependent substream
  (`strmtyp=1`) and a first audio frame without JOC are both normal cases.
* A container is only claimed when this component is ahead of the built-in container reader in
  Preferences → Decoding (see [Install](#containers-need-one-look-at-the-decoder-list)); the core
  does not let a decoder ask for a file another entry has already taken. Such a file's tags stay
  that reader's as well.
* Tags of a bare `.eac3` / `.ec3` (an ID3v2 tag in front of the stream, or an APEv2/ID3v1 tag
  behind it) can be **read but not written**: no component claims raw E-AC-3 for writing, and
  rewriting the whole file to insert a tag is not this component's job.
* Transport streams (`.ts`, `.m2ts`) are not claimed.
* **Force 24-bit output** only changes the precision the decoder delivers: foobar2000 converts the
  int24 it receives into its own floating-point samples, and which bit depth the output device runs
  at is still decided in Preferences → Output, not by this component. Samples outside ±1 follow the
  core's `continue` clip policy (binaural rendering can exceed full scale, so use **Gain** when
  attenuation is wanted).
* Playback length is exactly the file's duration. The binaural renderer still computes its room
  tail, but it is not delivered as playback time the file does not have.
* A seek re-enters the bitstream at the frame holding the target instead of decoding everything
  in front of it — which is why the cost of a seek does not depend on where it lands. The
  position is exact and does not drift; the samples are the same waveform handed to a decoder
  that started there, so they differ from a straight play-through by a low-level noise floor of
  −59 dBFS or quieter.

## Licence

`LICENSE` is the upstream MIT licence, copied unchanged; `kernel/` is a copy of the upstream
rendering core sources and keeps their notices. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
