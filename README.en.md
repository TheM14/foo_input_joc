# foo_input_joc — foobar2000 input component for E-AC-3 JOC

[简体中文](README.md) · [Install](#install) · [Settings](#settings) · [Building](#building) · [Known limitations](#known-limitations)

A foobar2000 input component for **E-AC-3 JOC (Dolby Atmos)** files: the JOC object and OAMD
metadata is taken from the E-AC-3 syncframes and paired with the 5.1 core PCM that ffmpeg
decodes, then rendered in real time to headphones (HRTF) or to a speaker layout up to 7.1. The
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

* **Output** — binaural, or a speaker layout from 2.0 to 7.1;
* **Binaural mode** (near / mid / far) and the room **tail** in seconds;
* **HRTF source** — a **SOFA** file or a **Rosella** `.personalized_headphone` model. An empty
  path means the default location, `<component directory>\HRTF\binaural.sofa` or
  `binaural.personalized_headphone`;
* **Gain** — a switch and a value in dB. Binaural rendering can exceed full scale on material
  that does not clip in the core mix, so attenuation belongs here;
* the **ffmpeg** executable to use.

No control on the page is disabled, and the status line states what is in effect.

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
render comparison, preferences-page layout check, container-probe check) and `tools\` the build
and test-bed scripts. Settings also read `JOC_*` environment overrides for one run (development
only); the list and what each one does is in `src\settings.cpp`.

To diagnose a problem, read `joc_decoder.log` beside the DLL: the component writes its own
version, the core version and the log path there at start-up.

## Known limitations

* ADM BWF output is not implemented.
* A container is only claimed when this component is ahead of the built-in container reader in
  Preferences → Decoding (see [Install](#containers-need-one-look-at-the-decoder-list)); the core
  does not let a decoder ask for a file another entry has already taken. Such a file's tags stay
  that reader's as well.
* Tags of a bare `.eac3` / `.ec3` (an ID3v2 tag in front of the stream, or an APEv2/ID3v1 tag
  behind it) can be **read but not written**: no component claims raw E-AC-3 for writing, and
  rewriting the whole file to insert a tag is not this component's job.
* Transport streams (`.ts`, `.m2ts`) are not claimed.
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
