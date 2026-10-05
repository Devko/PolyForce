# Roadmap

Status: ✅ done · 🔜 next · ⬜ planned · 💤 deferred

- [What's next](#whats-next)
- [Constraints](#constraints)
- [Milestones](#milestones)
- [Reviews](#reviews)
- [Deferred and not planned](#deferred-and-not-planned)
- [Decisions](#decisions)

---

## What's next

- ✅ **Skin render and page check** (2026-10-05, on a Force): every page draws and the sub-pages
  switch; the browser, selectors, a button and the wave view were checked. The first run found
  buttons dead after one press, steppers stalling in a turn (both fixed and covered by tests that
  send what the Force sends) and sub-pages showing one screen (fixed, see the
  [interface](INTERFACE.md#design) notes).
- ✅ **Release build** in CI against glibc 2.31, profile-guided, passing the plugin catalog's check;
  device bench of it: 11.1% p99 at 8 × 8 ([measurements](PERFORMANCE.md#device-measurements)).
- 🔜 **On the device:** play every page with Q-Links, save and reload a project, the large-table
  bench run (`make bench-device WAVETABLES=<folder>`).
- 🔜 **Milestone 1 device checks:** a large library (379 tables) reachable, CPU ≤ 15% at 8 × 8,
  project reload restores the tables; trace whether MPC calls `setParameter` right after restoring
  a project's state.
- 🔜 **Factory content on the device:** a listening pass through every category, and the built-in
  tables' build times (Growl, the slowest, at 73 ms on x86).
- 🔜 **Wave view on the device:** how quickly MPC redraws 48 meters at once, and that pushing them
  doesn't mark the project as changed.
- ⬜ **v0.1**, the first release: parameter list frozen (append-only from then on), catalog-style
  package.

## Constraints

What the device allows decides most of the design:

- **CPU:** 2902 µs per 128-frame block and instance; p99 ≤ 15% to pass. Every milestone ends with
  `make bench-device`. → [Performance](PERFORMANCE.md)
- **Skins:** static images, no text entry, no drawn graphics; popups only for fixed lists.
  → [Interface](INTERFACE.md#what-mpc-skins-can-do)
- **Projects** store parameter values by index: the parameter list becomes append-only at v0.1.
  Saved state uses real values by key, so ranges can still change.
  → [Architecture](ARCHITECTURE.md#parameters-and-saved-state)
- **Loading** a 256-frame table took 582 ms and 22 MB on the Force: loading must be off the audio
  thread, and tables must shrink. → [Performance](PERFORMANCE.md#wavetable-import)
- **Effects** are out of scope for now: MPC's insert effects follow the plugin on its track. The
  filter drive stays, as part of the voice.

---

## Milestones

### Phase 0 — spike ✅ (2026-10-04)

- ✅ 8 voices, 2 wavetable oscillators, unison up to 8 (detune, stereo width), level
- ✅ 4 built-in tables (Classic, PWM, Sync, Formant), 11 band-limited mip levels
- ✅ 2 filters (LP12/24, BP, HP12/24, notch, peak), drive, keytrack, serial/parallel
- ✅ Amp and mod envelope; env 2 → cutoff and wavetable position
- ✅ Serum-format WAV import (a 379-table library loads and plays)
- ✅ CPU meter in the status line, device bench, ASan test suite, install package

### Milestone 1 — wavetable library and browsing ✅

Design record: [M1_DESIGN.md](M1_DESIGN.md).

- ✅ Library: `<plugin folder>/Wavetables` and `/media/AkaiForce/Wavetables`; category = top
  folder, shared name prefixes hidden, built-ins as *Built-in*
- ✅ Per-instance loader thread, shared LRU cache (96 MB), lock-free handoff to the audio thread,
  150 ms debounce, `LOADING` / `MISSING` in the status line, a missing table's key kept
- ✅ Faster, smaller import: per-level mip lengths, two frames per FFT, 16-bit samples; a
  256-frame table now takes ~120 ms and 4.5 MB on x86 (was 582 ms and 22 MB on the Force)
- ✅ Recall by key (`o1_table=plugin:Analog/…`), not by index
- ✅ Browsing: table steppers, browser page, favorites, recent, random, copy / swap;
  `FRAME 37 / 256` on the position knob
- 🔜 On the device: see [What's next](#whats-next)

### Milestone 2 — voices ✅

- ✅ Steal modes Oldest · Quietest (released first) · Keep low · Keep high
- ✅ Click-free stealing: a stolen voice fades out over 3 ms before restarting
- ✅ Voice modes Poly · Duo · Mono · Legato (note stack: release returns to the held note)
- ✅ Glide: time or rate, always or legato only
- ✅ Same note again: retrigger or new voice
- ✅ Bend range up / down (0–24), velocity curve

### Milestone 3 — oscillators ✅

- ✅ Sub oscillator per oscillator: sine / triangle / saw / square, −36..+12 st, level
- ✅ Wave per oscillator: Table · Sine · Triangle · Saw · Square · Pulse (pulse width = position) ·
  Noise
- ✅ Phase offset, Reset / Random / Free
- ✅ Per-source routing (osc 1, osc 2, noise): F1 · F2 · F1+F2 · Direct; Serial / Parallel only
  decides whether F1 feeds F2
- ✅ Noise source with a continuous colour (dark … white … bright), loudness-compensated

### Milestone 4 — filters and engine modes ✅

- ✅ Comb+ / Comb− (feedback comb, cutoff = pitch) and Vowel (3 formants, cutoff morphs A-E-I-O-U)
- ✅ Engines Clean (no drift, linear) · Normal · Dirty (analog drift, saturation inside the filter
  loop per sample)
- ✅ Cutoff, resonance, drive as mod targets (with Milestone 5)
- ✅ NEON voice-parallel filters, four voices per vector; with the rest of the
  [NEON pass](PERFORMANCE.md#first-pass-neon) about −45% ARM instructions at 8 voices

### Milestone 5 — modulation ✅

- ✅ 2 LFOs: 7 shapes, free (0.02–40 Hz) or synced to MPC's tempo (17 divisions), phase, delay,
  fade-in, Retrig / Free / Global, unipolar switch, depth
- ✅ Mod matrix, 12 slots × 2 targets: source, via (amount scaled by a second source), modifier
  (curve, rectify, quantize, S&H, slew) with its amount
- ✅ 28 sources: envelopes, LFOs, velocity, note, mod wheel, aftertouch (channel and poly), bend,
  random, alternate, gate, step sequencer, 4 shapes, 4 XY pads (X and Y), breath, expression,
  constant
- ✅ 36 targets, including every envelope stage, LFO rate and depth, oscillator pitch / position /
  level / pan / detune, sub and noise levels, cutoff / resonance / drive, volume, pan
- ✅ 4 XY pads (8 Q-Link-friendly knobs) with auto-assign to free matrix slots

### Milestone 6 — sequencing ✅

- ✅ Arpeggiator: Up, Down, Up/Down, Down/Up, Played, Random, Chord; 1–4 octaves; latch; optional
  step pattern (rests, velocity and transposition from the step sequencer)
- ✅ 16-step sequencer: notes (transposed from the held key), velocity and a mod lane (a mod
  source); record steps from the keys
- ✅ 4 × 8-step shape sequencer (Step / Ramp / Smooth) as mod sources
- ✅ Rate, gate, swing; locked to MPC's transport when it plays

### Milestone 7 — presets and finish ✅

- ✅ 21 factory presets (embedded in the `.so`, validated by `surface.py` at build time); 205 since
  the [factory content](#factory-content--2026-10-04)
- ✅ Preset stepper and the browser page in PRESETS mode (categories, favorites, recent)
- ✅ Save preset: `<first preset root>/User/User NNN.pfp` (there is no text entry)
- ✅ Init patch, Randomize with an amount (volume and voicing untouched, attacks kept playable)
- ✅ Microtuning from `.tun` and `.scl` files (tuning stepper, saved with project and preset)
- ✅ Interface redesign: rounded cards on one dark ground, a teal accent, arc knobs (bipolar from the
  centre), short parameter names, both LFOs on one page, two shape lanes per page; post-build skin
  polish ([Interface](INTERFACE.md))
- ✅ Wave view (OSC → WAVES): both oscillators' current frames as 48 bars each

### Factory content ✅ (2026-10-04)

The tables and preset categories: [Factory content](FACTORY_CONTENT.md).

- ✅ 26 more built-in wavetables, 30 in all, computed (no sample data): analog (Square Sync, Reso Saw /
  Square, Harmonics, Comb Saw), digital (Fold, Phase Dist, CZ Reso, FM Ratio 1–3, FM Tine, Digital,
  Bitcrush, Chip), vocal (Vowels, Choir, Growl) and acoustic (Organ, E-Piano, Strings, Brass, Reed,
  Pluck, Mallet, Bell). The four originals are unchanged (bit for bit; Sync to 4e-16)
- ✅ Built on demand: Classic when the plugin loads (every slot's fallback), the others on the loader
  thread the first time a patch uses one, then cached like a file. Plugin load now builds one table
  instead of four
- ✅ 184 new factory presets, 205 in all, in 14 categories: one folder per category under
  `presets/Factory/`, the preset browser's categories. `surface.py` checks names (unique, short
  enough for a tile) and that every table a preset names is a built-in
- ✅ Level-matched: every preset rendered through the plugin and its volume set for −19 LUFS
  (one-shots −17 LUFS), peaks at most −3 dBFS; the first 21 kept their sound. 0.0.4: 6 dB louder
  (engine output), closer to MPC's own instruments; `make loudness` checks the levels
- ✅ The mod wheel does something on every new preset but the drums (filter, vibrato or position)
- ✅ Tests cover every built-in table and every factory preset (its tables load, it plays finite and
  audible)

### Second performance pass ✅ (2026-10-04)

ARM instructions per block against the NEON pass: 1 voice −16%, 8 voices −23%, 8 × 8 with a busy
matrix −11% ([details](PERFORMANCE.md#second-pass-control-rate-matrix-pgo)).

- ✅ 32-sample control rate; every control value glides across its chunk
- ✅ Cheaper matrix and LFOs: slots resolved per patch, shared sources per chunk, polynomial sine
- ✅ Profile-guided device build, trained under `qemu-arm`
- ✅ `pfbench`: per-pass times from a profiling build, a large table against one that fits the cache
- ✅ CPU guard: sheds the quietest release tails when an instance runs over budget, never held notes

### Third performance pass ✅ (2026-10-05)

ARM instructions per block against the build before it: positions holding still −17% (1 and 8
voices) to −30% (8 × 8), moving −4% (8 × 8) to −12% (1 voice), 8 × 8 with a busy matrix −17%
([details](PERFORMANCE.md#third-pass-output-stage-16-bit-tables-frame-cache)).

- ✅ Output stage four voices per vector
- ✅ 16-bit wavetables with a scale per frame: half the memory (4.5 MB per 256 frames)
- ✅ Frame cache: positions that hold still play a premixed float copy of their frame
- ✅ Comb lines no longer cleared at note start; parameters copied only after a write

---

## Reviews

### After Milestone 7 ✅ (2026-10-04)

Four parallel reviews (engine, sequencer and glue, surface and files, layout, build and docs);
about 60 confirmed findings fixed, each with a regression check, most in `test/review_test.cpp`.
The larger ones:

- chords into a full voice pool kept only their last note
- envelope knobs froze once the matrix touched an envelope stage
- generated notes hung on transport jumps, loops and new phrases
- a rate change stopped the arpeggiator or fired hundreds of steps
- a huge `*.wav` or a throw on the loader thread could take MPC down
- `make plugin-install` deleted the user's tables, presets and favorites
- preset loads could reach the audio thread half applied

Layout findings (popup lists past the screen edge, controls in card title bands, knob names) went
into the interface redesign.

### After the second performance pass ✅ (2026-10-05)

Four parallel reviews of the whole plugin; about 40 confirmed findings fixed, each with a regression
check (`test/review_test.cpp`, `m2`, `m5`). The larger ones:

- a 32-bit step index hung the audio thread on the Force at a large host song position
- a NaN from the host played NaN until reload
- a float WAV near `FLT_MAX` built a NaN table
- stuck notes with more than 16 keys (record, Off → Arp)
- a step replayed after a swing change; Play skipped step 0
- levels going to 0 stepped instead of gliding
- the loader thread could take MPC down on out-of-memory
- `make test-tables` failed every table

---

## Deferred and not planned

| Feature | Why not (now) |
|---|---|
| Built-in effects | MPC's insert effects do the job |
| More than 8 voices / 8 unison | 16 × 16 measured 45% of a block before the NEON pass; 8 × 8 stays the ceiling until the device bench of the current build says otherwise |
| Drag-and-drop modulation, scopes, a drawn wavetable display | MPC skins can't draw dynamic graphics (the wave view's bars are the workaround) |
| Wavetable scripting | No text entry on the device; at most an offline desktop tool that writes WAVs |
| MTS-ESP microtuning | Needs a tuning master plugin; none exists inside MPC |
| MPE | Untested whether MPC passes per-note channels and pitch bend to a VST2; probe before planning |
| Float tables, or float copies for moving positions | Twice the memory; the frame cache covers positions that hold still |
| Voices on a second core | MPC already spreads instances over its audio workers; a voice thread inside a 2.9 ms block risks dropouts that can't be tested without the device |

---

## Decisions

- 2026-10-04 — **Voices and unison capped at 8** (played on the device: 16 not needed; 8 × 8 = 15.2%).
- 2026-10-04 — **Browsing: all options** (knob scroll, browser page, favorites and recent, random,
  copy / swap).
- 2026-10-04 — **Table roots: plugin folder and SSD** (`/media/AkaiForce/Wavetables`; an absent
  drive just means no tables from it).
- 2026-10-04 — **Steal modes: Oldest, Quietest (released first), Keep low, Keep high.**
- 2026-10-05 — **Own repository** for PolyForce, with its full history.
- Sample wavetables used for testing are third-party content: never committed, never packaged.
