# foo_input_joc

foobar2000 input component for **E-AC-3 JOC (Dolby Atmos)** files: the JOC objects are
rendered to binaural (HRTF) or to a speaker layout up to 7.1, in real time.

Two files with the same name pay for the whole thing: `joc_core`'s C++ sources are copied
into [`kernel/`](kernel/) and compiled straight into the component, so there is nothing to
install beside `foo_input_joc.dll`.

## What it does

1. Finds the audio: a bare `.eac3` / `.ec3` stream is read as it is, while a container
   (`.mp4`, `.m4a`, `.m4b`, `.m4p`, `.m4r`, `.mov`, `.mkv`, `.mka`, `.webm`) is looked into
   first — the container's own headers say whether an E-AC-3 track is present and which
   audio track it is (MP4 sample entry `ec-3`, Matroska `CodecID A_EAC3`), and ffmpeg then
   copies that track out of the file byte for byte. The header walk is bounded and cheap, so
   an MP4 holding AAC is declined without starting anything.
2. Decides from the bitstream whether it really carries JOC (an EMDF container holding both
   the OAMD and the JOC payload — container metadata only ever says "E-AC-3", and the JOC
   flag inside it is frequently missing).
3. A file with no E-AC-3 track, or with one that carries no JOC, is handed back to
   foobar2000 with `exception_io_unsupported_format`, so the built-in decoder plays it — this
   component never decodes plain AC-3 or E-AC-3.
4. A JOC file is decoded as: the syncframes go to the renderer as metadata, the 5.1 core
   PCM comes from ffmpeg, and the renderer pairs them (one syncframe : 1536 bed samples)
   and produces the output PCM, which is handed back to foobar2000.

```
.eac3 / .ec3 file          container (.mp4 .mkv .m4a ...)
  │                          ├─ header walk (src/container_scan.cpp) ── no E-AC-3 ──▶ next decoder
  │                          └─ E-AC-3 track ── ffmpeg -c:a copy ──▶ syncframes
  ├─ JOC check (src/eac3_scan.cpp)  ─── no JOC ──▶ built-in E-AC-3 decoder
  └─ JOC
      ├─ syncframes ────────────────────▶ renderer metadata
      └─ ffmpeg -ac 6 -c:a pcm_f32le ───▶ 5.1 core PCM ──▶ renderer bed
                                                             │
                                                             ▼
                                                    2 ch or ≤7.1 PCM ──▶ foobar2000
```

## Repository layout

| Path | Contents |
|---|---|
| `kernel/` | Copy of the `joc_core` C++ sources (`include/` + `src/`) and `joc_kernel.vcxproj`, the static library the component links |
| `src/eac3_scan.*` | Syncframe walk and the JOC bitstream test |
| `src/container_scan.*` | Bounded header walk of MP4/MOV and Matroska: is there an E-AC-3 track, which one, and how long is the file |
| `src/joc_decode.*` | Decode engine: starts ffmpeg, drives the renderer, handles the end of stream. No foobar2000 headers, so it also builds into the offline tools |
| `src/input_joc.cpp` | The foobar2000 input: format recognition, yielding, `get_info`, `initialize`, `run` |
| `src/settings.*` | Configuration values and their environment overrides (development only) |
| `src/prefs.cpp`, `src/prefs.rc` | The preferences page |
| `src/log.*` | Diagnostic log written next to the DLL |
| `tests/` | Offline tools: bitstream self-test and cross-check against the renderer, render harness, preferences-page layout check, container-probe check |
| `tools/` | SDK fetch, build, package, deploy, unattended test bed run |

## Build

```powershell
pwsh -File tools/setup_sdk.ps1          # official SDK into SDK/, pinned to target 1.5/1.6
pwsh -File tools/build.ps1              # Win32 -> build\Win32\foo_input_joc.dll
pwsh -File tools/build.ps1 -Platform x64
pwsh -File tools/package.ps1            # both, packaged into dist\*.fb2k-component
```

`Release-Static` uses the static CRT (`/MT`); `/fp:precise` is required and must not be
changed. `foo_input_joc.vcxproj` builds `kernel\joc_kernel.vcxproj` first through a project
reference. The copied kernel sources are compiled with `JOC_STATIC` / `EJOC_STATIC` so their
entry points are neither imported nor exported.

## Install

Either drop `dist\foo_input_joc-<version>-<arch>.fb2k-component` onto foobar2000 (or use
Preferences → Components → Install), or copy `foo_input_joc.dll` into
`<profile>\user-components\foo_input_joc\`. The per-component subdirectory is required:
a DLL lying directly in `user-components\` is not scanned. 1.6 is 32-bit, 2.x ships both,
and a DLL of the wrong architecture is silently ignored.

`tools/deploy.ps1 -TestBed <path to portable foobar2000>` does the manual variant, and
`tools/run.ps1 -TestBed <path> -Play <file>` runs it unattended and prints the log.
Always let foobar2000 exit through `/exit`; a force-killed instance leaves a
`<profile>\running` marker behind and the next start then refuses to load any user
component.

### Containers need one look at the decoder list

foobar2000 tries the decoders in the order shown in Preferences → **Decoding** (the
"list of available decoders", where entries can be moved up and down). The built-in
container readers are in that list too, and when one of them is offered an MP4 or Matroska
file before this component, it takes the file and the JOC objects are lost — the file plays
as plain E-AC-3.

So, to play JOC from a container, move **JOC decoder (E-AC-3 JOC)** above **foobar2000 MP4
Demuxer** and **foobar2000 Matroska/WebM Reader** in that list. Nothing else is needed, and
bare `.eac3` / `.ec3` files are unaffected by the order. This is the same thing every
third-party decoder (the FFmpeg wrapper, for one) asks for, which is why the component does
not try to work around it. If a container still plays as plain E-AC-3, that list is where to
look.

## Settings

Preferences → Tools → **JOC decoder**:

* **Output** — binaural, or a speaker layout from 2.0 to 7.1;
* **Binaural mode** (near / mid / far) and the room **tail** in seconds;
* **HRTF source** — a **SOFA** file or a **Rosella** `.personalized_headphone` model. Leave
  the path empty to use the default location `<component directory>\HRTF\`:
  `binaural.sofa` or `binaural.personalized_headphone`;
* **Gain** — a switch plus a value in dB. Binaural rendering can exceed full scale on
  material that does not clip in the core mix, so attenuation belongs here;
* the **ffmpeg** executable to use.

Nothing on the page is disabled; the status line states what is in effect.

**HRTF data is not distributed with this repository.** A SOFA measurement set or a
personalised headphone model is supplied by whoever runs the component (and is listed in
`.gitignore` so it cannot be committed by accident). Speaker layouts and every offline test
except binaural rendering work without one; binaural rendering without an HRTF fails with a
message naming the file it looked for.

## Environment overrides

Development only: they override the stored settings for one run and every use is logged.
`JOC_OUTPUT`, `JOC_LAYOUT`, `JOC_HRTF`, `JOC_HRTF_SOURCE`, `JOC_BINAURAL_MODE`, `JOC_GAIN_DB`,
`JOC_GAIN_ENABLED`, `JOC_TAIL_SECONDS`, `JOC_OBJECT_DELAY`, `JOC_THREADS`, `JOC_FFMPEG`,
`JOC_LOG`.

## Known limitations

* ADM BWF output is not implemented.
* A container is only claimed when this component is ahead of the built-in container reader
  in Preferences → Decoding, as described under [Install](#install); the core does not let a
  decoder ask for a file another entry has already taken.
* Transport streams (`.ts`, `.m2ts`) are not claimed.
* The room tail is returned in full; the reference command-line renderer additionally trims
  trailing samples below a threshold, so its output can be shorter.
* x86 and x64 do not produce bit-identical binaural output (last-bit differences): the
  renderer's SIMD dispatch only applies to x86-64/ARM64, so 32-bit builds take the scalar
  path. The speaker path is bit-identical on both.

## Licence

`LICENSE` is the upstream MIT licence, copied unchanged; `kernel/` is a copy of the upstream
renderer sources and keeps their notices. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
