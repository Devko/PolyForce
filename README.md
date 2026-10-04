# PolyForce

A wavetable synth that runs **inside MPC on the Akai Force** as a native VST2 instrument,
modelled on the feature set of u-he Hive 2 (our own name, DSP, tables and presets; nothing of
u-he's is used). Working name; the plugin uid `PlFc` and `polyforce.so` may still change before
the first release. Effects are left to MPC's own insert effects.

- Roadmap and decisions: [docs/ROADMAP.md](docs/ROADMAP.md)
- Milestone 1 design record (library, loader, browser, push mechanics): [docs/M1_DESIGN.md](docs/M1_DESIGN.md)
- How MPC hosts plugins (ABI, skins, threads, device facts): `docs/MPC_PLUGIN_SPEC.md` in the
  RackForcePlugin project (kept with that project, not in this repository)

## Status (2026-10-04): Milestones 1–7 built, not yet on the device

Everything below passes the ASan/UBSan suite (753 checks) on x86 and the same suite
cross-compiled for the Force under qemu. A full code review after Milestone 7 fixed about 60
issues (voice stealing, envelope modulation, sequencer clock jumps, crash safety around files,
browser state, packaging); each fix has a regression check in `test/review_test.cpp`. The skin and the device bench still have to be run on the user's machine
(`make skin`, `make preview`, `make bench-device`).

- **Voices:** 8 voices · Poly / Duo / Mono / Legato · steal Oldest / Quietest / Keep low /
  Keep high with a 3 ms fade · same note retriggers or starts a new voice · glide (time or rate,
  always or legato) · bend range up/down · velocity curve
- **Oscillators (×2):** wavetable or classic wave (sine, triangle, saw, square, pulse, noise) ·
  unison up to 8 with detune and width · octave/semitone/fine · level, pan · phase offset and
  Reset / Random / Free · sub oscillator (4 waves, −36..+12 st) · route F1 / F2 / F1+F2 / Direct
- **Noise** with a continuous colour tilt and its own route
- **Wavetables:** 4 built-ins; Serum-format WAVs from the plugin folder and the SSD; loaded off
  the audio thread with a shared cache; table steppers, a browser page (categories, favorites,
  recent, random, copy, swap), `FRAME n / N` readout; saved by key with the project
- **Filters (×2):** Off, LP12, LP24, BP, HP12, HP24, Notch, Peak, Comb+, Comb−, Vowel · drive,
  keytrack, env 2 amount · serial or parallel · engine Clean / Normal / Dirty
- **Envelopes:** amp (with velocity) and mod (velocity, loop, → cutoff, → wavetable position)
- **Modulation:** 2 LFOs (7 shapes, free or tempo-synced, delay, fade, retrig / free / global,
  uni/bipolar) · a 12-slot matrix with 2 targets per slot, a "via" source and modifiers
  (curve, rectify, quantize, S&H, slew) · 29 sources, 37 targets · 4 XY pads with auto-assign
- **Sequencing:** arpeggiator (7 directions, 1–4 octaves, latch, step pattern) · 16-step note
  sequencer with record · 4 × 8-step shape sequencer as mod sources · gate and swing, synced to
  MPC's transport
- **Patch:** 21 factory presets, a preset browser, numbered user presets, Init, Randomize with
  an amount · microtuning from `.tun` and `.scl`
- Status line with live voice count, CPU and loading state; sustain pedal (holds the keys in
  Arp/Seq mode), CC 120/123, mod wheel, breath, expression, channel and poly aftertouch

CPU on the Force (p99, % of the 2.9 ms block, 2 oscillators, LP24+drive → LP12), measured on
v0.0.2, before the NEON pass; to be re-measured with this build (`make bench-device` runs the
plain cases and a busy mod matrix):

| Voices | ×1 unison | ×4 | ×8 |
|---|---|---|---|
| 4 | 4.3 | 5.8 | 7.8 |
| 8 | 8.2 | 11.4 | 15.2 |

### The NEON pass

The engine renders each 16-sample chunk in four passes over the sounding voices: prepare
(modulation, envelopes, oscillators into per-voice buses), filter 1, filter 2, output. The
buses hold the voices side by side per sample, so the state-variable filters run **four voices
per NEON vector** (`dsp/simd.h`: GCC vector types, NEON on the Force, SSE on x86, so the tests run
the same code). The wavetable oscillators and subs read four samples per step with 64-bit pair
loads and an unzip. Pitch, cutoff and pan use short polynomials instead of libm, the envelopes
run without a per-sample switch, and the plugin only rebuilds the patch and scans the
parameters for MPC when something changed.

Measured as ARM instructions per 128-frame block (the same `-O3 -mcpu` build, counted under
qemu; a proxy for the device, which `make bench-device` measures for real):

| Case | Before | After |
|---|---|---|
| 1 voice | 91.5 k | 56.8 k (−38%) |
| 8 voices × 1 | 439 k | 232 k (−47%) |
| 8 voices × 8 unison, busy matrix | 999 k | 567 k (−43%) |

The output matches the scalar engine to 2e-6 (x86) and 1.8e-4 (ARM, reciprocal estimates and
fused multiply-adds) relative RMS over 132 test scenes.

## Interface

The touchscreen pages are defined in `surface/surface.py` (`pages()`), after the approved design.

- **Look:** `style=td3` rounded cards on one flat ground (`#15181d`, the same colour as the card
  fill, so no control shows a box behind it), a teal accent (`#3fd0c0`), Titillium Web. Every tab
  has a header row (the status line and, on tabs with page modes, the mode selector on the right)
  over cards in two rows of 270 px or one of 552 px. Tabs: OSC, FILTER, MOD, MATRIX, BROWSE (MPC's
  first five), VOICE, SEQ.
- **Knobs** have a value arc from the minimum, or from 12 o'clock for bipolar parameters (pan,
  fine, amounts, ...). The generator draws one filmstrip per knob radius, so the radius picks the
  look (30, bipolar 29; small 22, bipolar 21): `KNOB_STYLES` in `surface.py` is the one place for it.
- **Names:** MPC shows a parameter's own name under its knob or slider and in its Q-Link overlay
  (not the layout's `label=`), so names are short and unique: at most 13 characters for knobs,
  sliders and toggles, and they must fit the label.
- **Q-Link sets** (their titles show in MPC's tab strip) only remap the Q-Links, the screen stays;
  each is named after what it controls.
- **Checks:** before writing anything `surface.py` checks the layout with the generator's own
  sizes (knob, slider and button boxes, enum labels, open popup lists), keeps controls and text out
  of the card title bands and bitmap text to the glyphs that font has.
- **Post-build polish:** `make skin` runs sd88me's `gen_vst.py` and then `surface/skin_polish.py`,
  which redraws the knob filmstrips (arc knobs), the trigger buttons (rounded, full size, real
  label; SAVE and AUTO-ASSIGN in the accent) and the stepper arrows (the generator cuts those of a
  stepper inside a page mode from the wrong image), keeping every file name and size. It checks the
  skin against `layout.conf` and `build/skin_style.json` first and fails the build on any mismatch.
  `python3 surface/skin_polish.py --selftest` runs it on a fabricated skin.
- `make skin` and `make preview` render the real skin and page previews; they build and run the
  vendored generator, so run them on the user's machine.

## Layout

```
surface/surface.py     THE source of the parameter list and the touchscreen pages: writes
                       params.json, layout.conf, vst.json, build/param_ids.h (ids, value curves,
                       limits) and build/factory_presets.h (presets/Factory embedded); checks the
                       layout and every factory preset before writing anything
surface/skin_polish.py redraws knob strips, buttons and stepper arrows after the generator (make skin)
dsp/wavetable.*        band-limited tables: 11 mip levels (2048 samples down to 256) per frame,
                       FFT-built two frames at a time; 4 built-ins; Serum WAV loader
dsp/synth.*            the engine: voices, oscillators (uint32 phase), sub, noise, Simper SVF,
                       comb and vowel filters, ADSRs, LFOs, the mod matrix; 16-sample control rate,
                       four voices per vector in the filters
dsp/simd.h             four-float vectors (NEON on the Force, SSE on x86)
dsp/mod.h              LFO shapes, sync divisions, mod sources, targets and modifiers
dsp/notegen.*          arpeggiator, step sequencer and shape sequencer on a beat clock
dsp/tuning.*           .tun / .scl parsing, 128-note pitch tables
plugin/plugin.cpp      VST2 glue: MIDI with sample offsets, transport, chunk state, denormal
                       flush, CPU meter
plugin/surface.*       the touchscreen side: parameter values, steppers, browser, push-backs to
                       MPC (audioMasterAutomate / UpdateDisplay from processReplacing only)
plugin/patch_map.*     0..1 <-> real values, display text, params -> Patch / SeqPatch
plugin/library.*       file libraries (tables, presets, tunings): scan, categories, favorites, recent
plugin/loader.*        the per-instance loader thread and the shared table cache
plugin/presets.*       preset and tuning libraries, user preset files
plugin/state.*         the state text shared by projects and preset files
plugin/paths.*         plugin dir, library roots, data dir, atomic file writes
plugin/vst2.h          hand-written VST2 ABI slice (from RackForcePlugin)
presets/Factory/       factory presets (NN_Name.pfp: NN orders them, "_" shows as a space)
test/                  the whole plugin through its VST2 entry points, ASan/UBSan, one file per
                       milestone plus review_test.cpp (host.h = a fake MPC host)
test/tables_sweep.cpp  every WAV in a folder: load, check, play, timing and memory
tools/bench.cpp        CPU bench: dlopen()s the .so like MPC, times every block
third_party/mpc-vst-plugins/   sd88me's MIT skin generator + installer (marked RackForce patches)
```

## Build and test (WSL, Ubuntu 24.04)

Needs g++ 13, `arm-linux-gnueabihf-g++` 13, GNU make ≥ 4.3, python3; for the skin and the
package also Pillow at `~/.venvs/rackforce/bin/python` (shared with RackForcePlugin); for
`test-arm` `qemu-user`.

```bash
wsl -e make -C /mnt/d/DEV/mockba/PolyForce test
```

| Target | What it does |
|---|---|
| `surface` | regenerate params, layout and the C++ headers from `surface/surface.py` (automatic) |
| `skin` / `preview` | the skin (TUI.json + PNGs) with sd88me's generator / the pages as `surface/build/page_*.png` |
| `test` | ASan/UBSan suite; uses `$(WAVETABLES)` (default `../wavetables`) for the import check |
| `test-arm` | the same suite built for the Force's CPU, run under `qemu-arm` |
| `test-tables` | load + play every WAV under `$(WAVETABLES)` |
| `bench` | x86 bench, only proves the bench works |
| `arm-plugin` | `build/arm/polyforce.so` for the Force |
| `bench-device FORCE=root@<ip>` | copies .so + bench + one 256-frame table to `/tmp`, runs on core 1, deletes them |
| `plugin-package` | `dist/PolyForce-<ver>-mpc-armv7.zip` with sd88me's installer; a reinstall keeps the user's Wavetables, Presets, Tunings and favorites/recent lists |
| `plugin-install FORCE=root@<ip>` | **run by the user**: stops MPC, edits `MPC.settings`, restarts MPC |

## Files on the device

| What | Where (the first root is where saving goes) | Override for tests |
|---|---|---|
| Wavetables `*.wav` | `<plugin dir>/Wavetables`, `/media/AkaiForce/Wavetables` | `PF_TABLE_ROOTS` |
| Presets `*.pfp` | `<plugin dir>/Presets` (user presets go to `User/`), `/media/AkaiForce/PolyForce Presets` | `PF_PRESET_ROOTS` |
| Tunings `*.tun`, `*.scl` | `<plugin dir>/Tunings`, `/media/AkaiForce/Tunings` | `PF_TUNING_ROOTS` |
| Favorites, recent | `<plugin dir>/*.txt` | `PF_DATA_DIR` |

## Rules worth knowing

- **Parameters:** free to change until v0.1, then append-only (MPC projects store values by
  index). Saved state is `polyforce 4` = key=value lines of *real* values plus the table,
  tuning and preset keys, so range changes don't remap saved projects; versions 1–3 are still read.
- **Limits:** `kMaxVoices`/`kMaxUnison` in `dsp/synth.h` must equal `MAX_VOICES`/`MAX_UNISON` in
  `surface.py`; enum lists in `surface.py` must match the C++ enums (static_asserts in
  `patch_map.cpp` check counts and order).
- **Real time:** nothing on the audio thread allocates, locks or throws; host callbacks only
  from `processReplacing`; a `try/catch` stands between every entry point and MPC. Files load
  on the instance's loader thread; the audio thread swaps a pointer.
- **The sample tables** in `D:\DEV\mockba\wavetables` (Echo Sound Works Core Tables, 379 WAVs,
  third-party) are test data: never committed, never packaged.
- The `.so` needs GLIBC_2.38 (`__isoc23_strtol`), the same as RackForce: fine on this Force,
  too new for MPC OS 2.x devices.
