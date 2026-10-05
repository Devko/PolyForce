# PolyForce roadmap

A Hive 2-class wavetable synth running inside MPC on the Akai Force (VST2, see
`docs/MPC_PLUGIN_SPEC.md` in the RackForcePlugin project). Our own name, DSP, presets and tables: the
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
| Measured 2026-10-04: 8 voices × 8 unison × 2 osc = 15.2%; filters ≈ 55% of a voice's cost | Filters were the first target (NEON, four voices per vector); since then the oscillators are ~70% at 8 × 8 (ARM instruction counts) |
| 256-frame table import on the Force: 582 ms, 22 MB | Loading must be off the audio thread; memory per table must shrink |
| Skins: static PNGs only, no text entry, no drawn waveforms | No wavetable display, no scopes, no drag-and-drop; text + knobs + tiles |
| `list` tiles and `stepper` text can change at runtime (dynamic name/display) | File names, folders, frame numbers can be shown as text |
| Popup option text is baked into PNGs | Popups only for fixed lists (filter types, mod sources, steal modes), never for files |
| Data wheel = 0.01 per click, Q-Link = 1/128 per detent, MPC sends "its value + delta" | Stepped params need `settle()` stepping (RackForce has it) to move exactly one step |
| Projects store parameter values by index | Parameter list becomes append-only at the first release (v0.1); until then free to change |
| Saved state = real values by key (`polyforce 4`) | Ranges can change without remapping saved projects |

---

## Phase 0 — spike ✅ (2026-10-04)

- ✅ 8 voices, 2 wavetable oscillators, unison up to 8 (detune, stereo width), level
- ✅ 4 built-in tables (Classic, PWM, Sync, Formant), 11 band-limited mip levels
- ✅ 2 filters (LP12/24, BP, HP12/24, notch, peak), drive, keytrack, serial/parallel
- ✅ Amp + mod envelope; env 2 → cutoff and wavetable position
- ✅ Serum-format WAV import (all 379 sample tables load and play)
- ✅ CPU meter in the status line, device bench, ASan test suite, install package

## Milestone 1 — wavetable library and browsing ✅

Design record: [M1_DESIGN.md](M1_DESIGN.md).

- ✅ Library: `<plugin dir>/Wavetables` and `/media/AkaiForce/Wavetables`, category = top
  folder, shared prefixes stripped from names, built-ins as "Built-in"
- ✅ Per-instance loader thread, shared LRU cache (96 MB), lock-free handoff to the audio
  thread, 150 ms debounce, `LOADING` / `MISSING` in the status line, missing key kept
- ✅ Faster, smaller import: per-level mip lengths and two frames per FFT; a 256-frame table
  now takes ~120 ms and 9.0 MB on x86 (-O2), was 582 ms and 22 MB on the Force (re-measure
  there: `make bench-device` times it)
- ✅ Recall by key (`o1_table=plugin:Analog/…`), not by index
- ✅ Browsing A–F: table steppers, browser page, favorites, recent, random, copy / swap;
  `FRAME 37 / 256` on the position knob
- ⬜ On the device: all 379 sample tables reachable, CPU ≤ 15% at 8 × 8, project reload

## Milestone 2 — voices ✅

- ✅ Steal modes Oldest · Quietest (released first) · Keep low · Keep high
- ✅ Click-free stealing: a stolen voice fades out over 3 ms before restarting
- ✅ Voice modes Poly · Duo · Mono · Legato (note stack: release returns to the held note)
- ✅ Glide: time or rate, always or legato only
- ✅ Same note again: retrigger or new voice
- ✅ Bend range up / down (0–24), velocity curve
- 💤 More than 8 voices: NEON filters are done; decide from the device bench of this build

## Milestone 3 — oscillators ✅

- ✅ Sub oscillator per oscillator: sine / triangle / saw / square, −36..+12 st, level
- ✅ Wave per oscillator: Table · Sine · Triangle · Saw · Square · Pulse (pulse width =
  position) · Noise
- ✅ Phase offset, Reset / Random / Free
- ✅ Per-source routing (osc 1, osc 2, noise): F1 · F2 · F1+F2 · Direct; Serial/Parallel only
  decides whether F1 feeds F2
- ✅ Noise source with a continuous colour (dark … white … bright), loudness-compensated
- 💤 Wavetable scripting (Hive's `.uhm`): as an offline desktop tool that writes WAVs, if ever

## Milestone 4 — filters and engine modes ✅

- ✅ Comb+ / Comb− (feedback comb, cutoff = pitch) and Vowel (3 formants, cutoff morphs A-E-I-O-U)
- ✅ Engines Clean (no drift, linear) · Normal · Dirty (analog drift, saturation inside the
  filter loop per sample)
- ✅ Cutoff, resonance, drive as mod targets (Milestone 5)
- ✅ **NEON voice-parallel filters**: four voices per vector (lane-packed buses), drive as its
  own pass; with NEON oscillator reads, fast pitch/cutoff math and per-block caching about
  −45% ARM instructions at 8 voices (README, "The NEON pass"). Device numbers: `make bench-device`.

## Milestone 5 — modulation ✅

- ✅ 2 LFOs: 7 shapes, free (0.02–40 Hz) or synced to MPC's tempo (17 divisions), phase,
  delay, fade-in, Retrig / Free / Global, unipolar switch, depth
- ✅ Mod matrix, 12 slots × 2 targets: source, via (amount scaled by a second source),
  modifier (curve, rectify, quantize, S&H, slew) with its amount; popups for the lists
- ✅ 29 sources: envelopes, LFOs, velocity, note, mod wheel, aftertouch (channel and poly),
  bend, random, alternate, gate, step sequencer, 4 shapes, 4 XY pads, breath, expression,
  constant
- ✅ 37 targets including every envelope stage, LFO rate and depth, oscillator pitch /
  position / level / pan / detune, sub and noise levels, cutoff / resonance / drive, volume, pan
- ✅ 4 XY pads (8 Q-Link-friendly knobs) with auto-assign to free matrix slots

## Milestone 6 — sequencing ✅

- ✅ Arpeggiator: Up, Down, Up/Down, Down/Up, Played, Random, Chord; 1–4 octaves; latch;
  optional step pattern (rests, velocity and transposition from the step sequencer)
- ✅ 16-step sequencer: notes (transpose from the held key), velocity and a mod lane (a mod
  source); record steps from the keys
- ✅ 4 × 8-step shape sequencer (Step / Ramp / Smooth) as mod sources
- ✅ Rate, gate, swing; locked to MPC's transport (`audioMasterGetTime` ppq) when it plays

## Milestone 7 — presets and finish ✅ (release ⬜)

- ✅ 21 factory presets (embedded in the .so, validated by `surface.py` at build time)
- ✅ Preset stepper and the browser page in PRESETS mode (categories, favorites, recent)
- ✅ Save preset: `<first preset root>/User/User NNN.pfp` (there is no text entry)
- ✅ Init patch, Randomize with an amount (volume and voicing untouched, attacks kept playable)
- ✅ Microtuning from `.tun` and `.scl` files (tuning stepper, saved with project and preset)
- ✅ Interface redesign: rounded cards on one dark ground, a teal accent, arc knobs (bipolar
  from the centre), short parameter names for MPC's labels, BROWSE in the first five tabs, both
  LFOs on one page, two shape lanes per page; `surface/skin_polish.py` redraws knobs, buttons and
  stepper arrows after the generator (README, "Interface")
- ✅ Wave view (OSC tab, WAVES): both oscillators' current frames as 48 bars each (display-only
  meters the plugin sets while the page shows; RackForce patch 5 to the generator, the bars drawn by
  skin_polish.py). On the device: how quickly MPC redraws 48 meters at once, and that pushing them
  doesn't mark the project as changed
- ⬜ Skin render and page check on the user's machine (`make skin`, `make preview`; the first
  real run of skin_polish.py and of the meter patch), then the device: install, play every page,
  `make bench-device`
- ⬜ First release v0.1: parameter list frozen (append-only from then on), catalog-style package

---

## Review after Milestone 7 ✅ (2026-10-04)

Four parallel reviews (engine, sequencer + glue, surface + files, layout + build + docs); every
confirmed finding fixed with a regression check in `test/review_test.cpp`. The larger ones:
chords into a full voice pool kept only their last note; envelope knobs froze once the matrix
touched an envelope stage; generated notes hung on transport jumps, loops and new phrases; a rate
change stopped the arp or fired hundreds of steps; a huge `*.wav` or a throw on the loader thread
could end MPC; `make plugin-install` deleted the user's tables, presets and favorites; preset
loads could reach the audio thread half applied. Layout findings (popup lists past the screen
edge, controls in frame title bands, knob names) go into the interface redesign.

---

## Performance, second pass ✅ (2026-10-04)

ARM instructions per block against the NEON pass: 1 voice −16%, 8 voices −23%, 8 × 8 with a
busy matrix −11% (README, "The second pass").

- ✅ 32-sample control rate; every control value glides across its chunk (no zipper from the
  coarser rate; a regression check holds a 40 Hz LFO on level, volume and pan to the waveform's
  own slope)
- ✅ Cheaper matrix and LFOs: slots resolved per patch, shared sources per chunk, polynomial sine
- ✅ Profile-guided device build, trained under qemu-arm (`PGO=0` for the plain one)
- ✅ pfbench: per-pass times from a profiling build, a large table against one that fits the cache
- ✅ CPU guard: sheds the quietest release tails when an instance runs over budget; never held notes
- ✅ Second review (2026-10-05): four parallel reviews of the whole plugin after the pass; about
  40 confirmed findings fixed, each with a regression check (`test/review_test.cpp`, m2, m5).
  The larger ones: a 32-bit step index hung the audio thread on the Force at a large host song
  position; a NaN from the host played NaN until reload; a float WAV near FLT_MAX built a NaN
  table; stuck notes with more than 16 keys (record, Off → Arp); a step replayed after a swing
  change; Play skipped step 0; levels going to 0 stepped instead of gliding; the loader thread
  could end MPC on out-of-memory; `make test-tables` failed every table
- 🔜 Device: `make bench-device WAVETABLES=<folder>`, add the row to the bench record, read the
  large-table result
- ⬜ 16-bit tables (half the memory traffic), only if the large table clearly costs more in the
  sources pass on the device
- 💤 Precomputed float frame pairs: about one instruction in eleven for twice the table memory
- 💤 Voices on a second core: MPC already spreads instances over its audio workers; a voice
  thread inside a 2.9 ms block risks dropouts that can't be tested without the device

---

## Deferred / not planned 💤

| Hive 2 feature | Why not (now) |
|---|---|
| 7 built-in effects | MPC's insert effects do the job |
| Drag-and-drop modulation, scopes, wavetable view | MPC skins can't draw dynamic graphics |
| MTS-ESP microtuning | Needs a tuning master plugin; none exists inside MPC |
| MPE | Untested whether MPC passes per-note channels/pitch bend to a VST2 — probe before planning |
| 16 voices × 16 unison | Measured 45% of a block before the NEON pass; 8 × 8 stays the ceiling until the device bench of this build says otherwise |
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

- ✅ Under version control: `PolyForce/` in Devko/RackForce.
- ⬜ Add a row to the bench record for the M1–M7 build (`make bench-device`).
- ⬜ Install on the device and confirm it plays (user: `make plugin-install`).
- The ESW sample tables (`D:\DEV\mockba\wavetables`) are third-party test data: never committed,
  never packaged.

---

## Decisions

- 2026-10-04 — **Voices and unison capped at 8** (played on the device: 16 not needed; 8 × 8 = 15.2%).
- 2026-10-04 — **Browsing: all of 1.5 A–F** (knob scroll, browser page, favorites + recent,
  random + copy/swap).
- 2026-10-04 — **Table roots: plugin folder + SSD** (`/media/AkaiForce/Wavetables`; absent drive = no tables from it).
- 2026-10-04 — **Steal modes: Oldest, Quietest (released first), Keep lowest, Keep highest.**
