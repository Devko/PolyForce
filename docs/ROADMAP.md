# PolyForce roadmap

A Hive 2-class wavetable synth running inside MPC on the Akai Force (VST2, see
`../RackForcePlugin/docs/MPC_PLUGIN_SPEC.md`). Our own name, DSP, presets and tables: the
feature set is the reference, nothing of u-he's is copied.

**Effects are out of scope for now:** MPC's own insert effects follow the plugin on its track.
The filter drive stays (it is part of the voice, not an effect).

Status: ✅ done · 🔜 next · ⬜ planned · 💤 deferred

---

## Ground rules from the device

These numbers and limits decide most of the design below.

| Fact | Consequence |
|---|---|
| 2902 µs per 128-frame block, per instance; catalog PASS = p99 ≤ 15% | Every milestone ends with `make bench-device`; 8 voices × 8 unison must stay ≤ 15% |
| Measured 2026-10-04: 8 voices × 8 unison × 2 osc = 15.2%; filters ≈ 55% of a voice's cost | Filters are the optimisation target, not oscillators |
| 256-frame table import on the Force: 582 ms, 22 MB | Loading must be off the audio thread; memory per table must shrink |
| Skins: static PNGs only, no text entry, no drawn waveforms | No wavetable display, no scopes, no drag-and-drop; text + knobs + tiles |
| `list` tiles and `stepper` text can change at runtime (dynamic name/display) | File names, folders, frame numbers can be shown as text |
| Popup option text is baked into PNGs | Popups only for fixed lists (filter types, mod sources, steal modes), never for files |
| Data wheel = 0.01 per click, Q-Link = 1/128 per detent, MPC sends "its value + delta" | Stepped params need `settle()` stepping (RackForce has it) to move exactly one step |
| Projects store parameter values by index | Parameter list becomes append-only at the first release (v0.1); until then free to change |
| Saved state = real values by key (`polyforce 2`) | Ranges can change without remapping saved projects |

---

## Phase 0 — spike ✅ (2026-10-04)

- ✅ 8 voices, 2 wavetable oscillators, unison up to 8 (detune, stereo width), level
- ✅ 4 built-in tables (Classic, PWM, Sync, Formant), 11 band-limited mip levels
- ✅ 2 filters (LP12/24, BP, HP12/24, notch, peak), drive, keytrack, serial/parallel
- ✅ Amp + mod envelope; env 2 → cutoff and wavetable position
- ✅ Serum-format WAV import (all 379 sample tables load and play)
- ✅ CPU meter in the status line, device bench, ASan test suite, install package

---

## Milestone 1 — wavetable library and browsing 🔜

Goal: pick any table on the device in seconds, by touch or by knob, and get it back when the
project reloads. **Full design, build order and tests: [M1_DESIGN.md](M1_DESIGN.md).**

### 1.1 Library
- 🔜 Table folders: `<plugin dir>/Wavetables/<Category>/…/*.wav` (shipped with the plugin;
  user tables copied there), plus an optional second root on the SSD
  (`/media/AkaiForce/Wavetables`; the SSD is `noexec`, which only matters for binaries).
- 🔜 Category = top-level folder; deeper folders fold into it. Built-ins appear as category
  "Built-in". Sorted by name.
- 🔜 Scan at plugin load on a worker thread: file names and frame counts from the WAV header
  only, no FFTs (a few ms for 400 files).

### 1.2 Loading
- 🔜 Worker thread loads; the audio thread swaps the table pointer at a block boundary; the old
  table is freed off the audio thread. The old sound keeps playing until the new table is ready.
- 🔜 One shared, ref-counted cache per process: the same table on both oscillators (or in two
  instances) is loaded once. Least-recently-used tables are evicted beyond a memory cap.
- 🔜 Status line shows `LOADING <name>` / `MISSING <name>`.
- 🔜 Debounce: while scrolling quickly, load only the table the scroll stops on (~250 ms).

### 1.3 Faster, smaller import
Targets for a 256-frame table on the Force: under 150 ms, under 6 MB.
- ⬜ High mip levels stored shorter (level k at max(2048 >> k, 256) samples): about 4× less
  memory and FFT work, inaudible (those levels are heavily oversampled today).
- ⬜ Real-input float FFT instead of complex double.
- ⬜ Measure with `pfbench -t` before and after.

### 1.4 Recall
- 🔜 The state saves the table's path relative to its root (not an index: indices move when
  files are added). A missing file falls back to Built-in/Classic and says so.

### 1.5 Browsing (pick from the options in "Decisions" below)
- **A. Knob scroll** — per oscillator, a `TABLE` stepper showing `Category / Name`; prev/next
  arrows; the data wheel or a Q-Link steps one table per click, debounced load.
- **B. Browser page** — a dedicated page: category tiles on the left, a page of table-name
  tiles on the right (e.g. 3 × 6), page arrows, an OSC 1 / OSC 2 target switch; tap = load,
  the loaded table lit.
- **C. Favorites** — a star button on the current table; a "★ Favorites" pseudo-category;
  kept in `favorites.txt` next to the plugin, shared by every project.
- **D. Recent** — the last 12 loaded tables as a pseudo-category.
- **E. Random** — a dice button: random table from the current category (or everything).
- **F. Copy / swap** — copy OSC 1's table to OSC 2, or swap them.
- Always shown, whatever is picked: `FRAME 37 / 256` next to the position knob (text, since
  no waveform drawing is possible).

### 1.6 Done when
- Every one of the 379 sample tables can be reached and loaded on the device, the CPU stays
  ≤ 15% at 8 × 8, saving and reloading a project restores both tables, and a missing table
  doesn't crash or silence the plugin.

---

## Milestone 2 — voices 🔜 (steal modes) / ⬜

- 🔜 **Steal mode** parameter (popup): e.g. Oldest · Quietest (released first) · Keep lowest
  note · Keep highest note. One of them is written by you (`Synth::victim()`).
- ⬜ **Click-free stealing**: a stolen voice fades out in ~3 ms before restarting.
- ⬜ **Voice modes**: Poly · Duo · Mono · Legato (Hive: poly up to 16, duophonic, mono, legato).
- ⬜ **Glide**: time or rate mode, always / legato only.
- ⬜ **Same note again**: retrigger the voice (now) or start a new one.
- ⬜ Pitch-bend range, velocity curve; aftertouch and mod wheel as mod sources (Milestone 5).
- 💤 More than 8 voices: revisit after the NEON filter work (4.3).

---

## Milestone 3 — oscillators ⬜

- ⬜ Sub oscillator per oscillator: waveform, octave (-1/-2), level (Hive: tunable sub).
- ⬜ Classic-wave mode next to wavetable mode: sine, triangle, saw, square, pulse (width),
  noise… (Hive: 9 waveforms per oscillator).
- ⬜ Phase: free / reset / random, phase offset, unison phase spread.
- ⬜ Per-oscillator filter routing (F1, F2, both, none) replacing the global serial/parallel.
- ⬜ Noise source with colour.
- 💤 Wavetable scripting (Hive's `.uhm`): as an offline desktop tool that writes WAVs, if ever.

---

## Milestone 4 — filters and engine modes ⬜

- ⬜ Comb filter (+/- feedback), vowel/formant filter (Hive: comb, "reverb", dissonant, sideband).
- ⬜ Engine modes **Clean / Normal / Dirty**: Clean = today's linear SVF, Dirty = saturation
  inside the filter loop. Doubles as a CPU setting.
- ⬜ **NEON voice-parallel filters**: 4 voices per SIMD lane group; the biggest CPU win available.
- ⬜ Filter cutoff/resonance as mod targets (with Milestone 5).

---

## Milestone 5 — modulation ⬜

- ⬜ 2 LFOs: shapes (sine, tri, saw up/down, square, S&H, smooth random), rate free or synced to
  MPC's tempo (`audioMasterGetTime`), delay/fade-in, per-voice or global, unipolar switch.
- ⬜ Mod matrix, Hive-style 12 slots × 2 targets: source, target, amount, modifier (curve,
  rectify, quantize, S&H, slew). Touch UI = popups for source/target (fixed lists suit them).
- ⬜ Sources: env 1/2, LFO 1/2, velocity, keytrack, mod wheel, aftertouch, pitch bend, random
  per note, step/shape sequencers, XY.
- ⬜ Envelope stages as targets (Hive: every ADSR stage modulatable).
- ⬜ 4 XY controls → Q-Link pairs, auto-assigned to unused targets (Hive: XY auto-assign).

---

## Milestone 6 — sequencing ⬜

- ⬜ Arpeggiator: up to 3 octaves, direction, order, frame/restart, synced to MPC transport.
- ⬜ 16-step sequencer: notes or modulation (Hive: can also run as a mod source without notes).
- ⬜ 8-step shape sequencer with 4 outputs.
- Reuse RackForce's step-grid touch patterns and transport handling.

---

## Milestone 7 — presets and finish ⬜

- ⬜ Own preset library (sound design is the long job; no u-he presets, ever).
- ⬜ Preset browser: same category + tile pattern as the table browser (1.5 B).
- ⬜ Save preset to SD: auto-named (`User 001`, or `<table> <category> 001`): there is no text entry.
- ⬜ Init patch, randomise patch.
- ⬜ Microtuning from `.tun` files.
- ⬜ First release v0.1: parameter list frozen (append-only from then on), catalog-style package.

---

## Deferred / not planned 💤

| Hive 2 feature | Why not (now) |
|---|---|
| 7 built-in effects | MPC's insert effects do the job |
| Drag-and-drop modulation, scopes, wavetable view | MPC skins can't draw dynamic graphics |
| MTS-ESP microtuning | Needs a tuning master plugin; none exists inside MPC |
| MPE | Untested whether MPC passes per-note channels/pitch bend to a VST2 — probe before planning |
| 16 voices × 16 unison | Measured 45% of a block; 8 × 8 is the practical ceiling until 4.3 |
| `.uhm` wavetable scripting on the device | No text entry; at most an offline tool |

---

## Bench record (Force, `make bench-device`, p99 % of the 2902 µs block)

Patch: both oscillators, F1 LP24 + drive → F2 LP12, held chords, bench pinned to core 1 with
MPC running. PASS ≤ 15%, WARN ≤ 35%.

| Date | Build | 8v ×1 | 8v ×4 | 8v ×8 | 16v ×16 | 256-frame import |
|---|---|---|---|---|---|---|
| 2026-10-04 | Phase 0 (16-voice engine) | 8.1 | 11.5 | 15.5 | 45.1 | 582 ms, 22 MB |
| 2026-10-04 | v0.0.2 (8 × 8 cap) | 8.2 | 11.4 | 15.2 | — | (same code) |

Other Phase 0 numbers: `VSTPluginMain` (builds the 4 built-in tables) 163 ms on the Force;
x86 sweep of all 379 ESW tables: all load and play, 334 ms average import, worst 468 ms.

---

## Housekeeping

- ⬜ `git init` the repo and commit Phase 0 (it is not under version control yet).
- ⬜ Add a row to the bench record at the end of each milestone.
- ⬜ Install v0.0.2 on the device and confirm it plays (user: `make plugin-install`).
- The ESW sample tables (`D:\DEV\mockba\wavetables`) are third-party test data: never committed,
  never packaged.

---

## Decisions

- 2026-10-04 — **Voices and unison capped at 8** (played on the device: 16 not needed; 8 × 8 = 15.2%).
- 2026-10-04 — **Browsing: all of 1.5 A–F** (knob scroll, browser page, favorites + recent,
  random + copy/swap).
- 2026-10-04 — **Table roots: plugin folder + SSD** (`/media/AkaiForce/Wavetables`; absent drive = no tables from it).
- 2026-10-04 — **Steal modes: Oldest, Quietest (released first), Keep lowest, Keep highest.**
