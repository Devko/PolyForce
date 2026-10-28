# Performance

- [The budget](#the-budget)
- [Device measurements](#device-measurements)
- [First pass: NEON](#first-pass-neon)
- [Second pass: control rate, matrix, PGO](#second-pass-control-rate-matrix-pgo)
- [Third pass: output stage, 16-bit tables, frame cache](#third-pass-output-stage-16-bit-tables-frame-cache)
- [Fourth pass: filter step, bus transpose, unison stacks](#fourth-pass-filter-step-bus-transpose-unison-stacks)
- [Memory traffic](#memory-traffic)
- [CPU guard](#cpu-guard)
- [Wavetable import](#wavetable-import)
- [Considered and left out](#considered-and-left-out)

---

## The budget

MPC renders 128-frame blocks: **2902 µs per block**, per plugin instance. A plugin passes at
**p99 ≤ 15%** of the block (warns up to 35%). PolyWerk's ceiling, **8 voices × 8 unison** with
both oscillators, has to stay within 15%.

Every optimisation is checked two ways:

- **On the device** with `make bench-device` (see [Building](BUILDING.md#benchmarking-on-the-device)).
- **Between device runs**, as ARM instructions per 128-frame block: the same `-O3 -mcpu` build,
  counted under `qemu-arm`. A proxy for the device, not a substitute.

The output of every optimised path is compared with the plain scalar engine: within 2e-6 relative
RMS on x86, 1.8e-4 on ARM (reciprocal estimates and fused multiply-adds), over 132 test scenes.

## Device measurements

`make bench-device`, p99 in % of the 2902 µs block. Patch: both oscillators, filter 1 LP24 + drive
→ filter 2 LP12, held chords; bench pinned to core 1 with MPC running.

| Date | Build | 8 v × 1 | 8 v × 4 | 8 v × 8 | 16 v × 16 | 256-frame import |
|---|---|---|---|---|---|---|
| 2026-10-04 | Phase 0 (16-voice engine) | 8.1 | 11.5 | 15.5 | 45.1 | 582 ms, 22 MB |
| 2026-10-04 | v0.0.2 (8 × 8 cap) | 8.2 | 11.4 | 15.2 | — | (same code) |
| 2026-10-05 | v0.0.3 release build (CI: GCC 11, glibc 2.31, profile-guided; all three passes) | 5.3 | 7.7 | 11.1 | — | — |
| 2026-10-26 | v0.0.5 before the fourth pass (GCC 13, profile-guided) | 4.6 | 7.7 | 11.8 | — | 209 ms, 4.5 MB |
| 2026-10-26 | Fourth pass (same build setup, benched right after the line above) | 4.3 | 6.4 | 9.2 | — | (same code) |

By voice count:

| Voices | v0.0.2 × 1 | × 4 | × 8 | v0.0.3 × 1 | × 4 | × 8 |
|---|---|---|---|---|---|---|
| 1 | | | | 1.7 | 1.9 | 2.3 |
| 2 | | | | 2.4 | 3.0 | 3.7 |
| 4 | 4.3 | 5.8 | 7.8 | 2.9 | 4.2 | 5.9 |
| 8 | 8.2 | 11.4 | 15.2 | 5.3 | 7.7 | 11.1 |

v0.0.3 with a busy matrix (8 slots, both LFOs): 6.2 at 8 × 1, 11.2 at 8 × 8; the worst single block
11.9%. Every case passes. The release `.so` loads (`VSTPluginMain`, Classic built) in 42 ms. The
three passes below came between the two builds: at 8 × 8 the block costs 27% less than v0.0.2's.

Other Phase 0 numbers: `VSTPluginMain` (then building the four built-in tables) took 163 ms on the
Force; it now builds only Classic and the classic oscillator shapes, the other built-ins on first
use (6–73 ms each on x86).
An x86 sweep of 379 commercial sample tables: all load and play, 334 ms average import, 468 ms worst.

## First pass: NEON

At v0.0.2 the filters were about 55% of a voice's cost, so they came first.

The engine renders each control chunk in passes over the sounding voices: control (matrix, mod
envelope, pitch), sources (oscillators, subs, noise into per-voice buses), filter 1, filter 2,
output. The buses hold the voices side by side per sample, so the state-variable filters run
**four voices per NEON vector** (`dsp/simd.h`: GCC vector types, NEON on the Force, SSE on x86, so
the tests run the same code). Drive is a pass of its own.

Also in this pass:

- the wavetable oscillators and subs read four samples per step with 64-bit pair loads and an unzip
- pitch, cutoff and pan use short polynomials instead of libm
- the envelopes run without a per-sample switch
- the plugin only rebuilds the patch and rescans the parameters for MPC when something changed

| ARM instructions per block | Before | After |
|---|---|---|
| 1 voice | 91.5 k | 56.8 k (−38%) |
| 8 voices × 1 | 439 k | 232 k (−47%) |
| 8 voices × 8 unison, busy matrix | 999 k | 567 k (−43%) |

## Second pass: control rate, matrix, PGO

- **32-sample control rate with glides.** The matrix, the envelopes' control outputs, filter
  coefficients and wave position are computed every 32 samples instead of 16, and every value
  glides across the chunk it applies to instead of stepping:
  - oscillator level × pan, morph, sub, noise and the voice gain: per sample
  - the state-variable filters' g, k and drive: every 16 samples, coefficients rebuilt per step so
    the filter stays stable
  - comb delay and feedback: per sample; vowel formants: every 16 samples

  A 40 Hz LFO on level, volume or pan moves the output no faster than the waveform itself (a check
  in `test/m5_test.cpp` that the old engine fails). Time constants stay referenced to 16 samples,
  so envelopes, smoothing and drift keep their timing.
- **Cheaper matrix and LFOs:** slots resolved once per patch, shared sources once per chunk, a
  polynomial LFO sine.
- **Profile-guided build** (`make arm-plugin` when `qemu-arm` is installed): an instrumented copy
  plays 132 patches under qemu (`tools/pgo_train.cpp`, about 10 s), then the `.so` is compiled
  with that profile. Functions the trainer never ran are optimised as usual; inside the ones it
  ran, paths it never took (other LFO shapes, glide, mono) count as cold, so the trainer covers the
  common patches broadly. `PGO=0` builds without; `make test-arm-pgo` runs the suite against the
  shipped objects.

| ARM instructions per block | NEON pass | + control rate, matrix | + PGO |
|---|---|---|---|
| 1 voice | 56.8 k | 52.1 k (−8%) | 47.7 k (−16%) |
| 8 voices × 1 | 232 k | 193 k (−17%) | 178 k (−23%) |
| 8 voices × 8 unison, busy matrix | 567 k | 518 k (−9%) | 505 k (−11%) |

## Third pass: output stage, 16-bit tables, frame cache

- **Output stage across voices.** The last pass (amp envelope, steal fade, gliding gains, the sum
  into the output) runs **four voices per vector** like the filters, and four samples' sums are
  reduced into one vector. Up to four voices the envelope's closed form `c1 + c2 · q^(i+1)` is
  worked out inside the sum; with more voices, or in a chunk with a steal fade or a stage change,
  it goes through a table (eight lanes of everything would not fit NEON's registers).
- **16-bit wavetables.** Samples are `int16` with one float scale per frame (the largest value of
  the frame over all its levels maps to 32767): a 256-frame table takes **4.5 MB instead of 9.0 MB**,
  so the shared 96 MB table cache holds twice as many, and the built-ins shrink the same way. The
  rounding sits about 96 dB under each frame's own peak, so a quiet frame keeps its resolution.
  The importer quantises frame by frame (no full-size float copy while loading). While the
  position moves, the oscillator reads the 16-bit frames directly: one 32-bit load per pair of
  neighbouring samples, unzip, widen, convert, about as many instructions per sample as the float
  tables took.
- **Frame cache for positions that hold still.** A position that is the same as in the previous
  chunk plays a float copy of its frame at that mip level, two frames premixed at the morph: one
  table read per sample instead of two, nothing to convert. 24 slots per instance (about 200 KB),
  shared by its voices, least recently used goes first, each voice remembering its last slot; at
  most two full-size frames are (re)filled per 32-sample chunk, past that the oscillator reads the
  table directly. Sub oscillators use it too.
- **Comb filters** no longer clear their delay lines (64 KB per voice) when a note starts: a read
  further back than the note has written counts as silence.
- **The plugin copies the parameters** for the engine only when one was written since the last
  block.

ARM instructions per block, counted with the same patch before and after (both oscillators, F1
LP24 with drive into F2 LP12, notes held; "moving": envelope 2 sweeps the position, "still": it
stays put):

| ARM instructions per block | Before | After | PGO before | PGO after |
|---|---|---|---|---|
| 1 voice, moving | 56.3 k | 49.3 k (−12%) | 52.8 k | 45.6 k (−14%) |
| 1 voice, still | 57.0 k | 47.2 k (−17%) | 52.8 k | 43.2 k (−18%) |
| 8 voices × 1, moving | 198.5 k | 183.3 k (−8%) | 184.8 k | 169.6 k (−8%) |
| 8 voices × 1, still | 198.7 k | 164.7 k (−17%) | 183.2 k | 150.0 k (−18%) |
| 8 × 8 unison, moving | 496.2 k | 477.3 k (−4%) | 483.2 k | 476.6 k (−1%) |
| 8 × 8 unison, still | 497.3 k | 349.2 k (−30%) | 481.2 k | 332.7 k (−31%) |
| 8 × 8 unison, busy matrix | 528.0 k | 440.8 k (−17%) | 509.3 k | 434.1 k (−15%) |

(In the busy patch one oscillator's modulated position rests at the end of its table, so it plays
from the cache.) qemu counts no memory stalls: what half the table memory does for cache misses
only the device bench shows.

The 16-bit samples are the one deliberate change in the sound: over the 132 test scenes the output
is within about 1e-5 relative RMS of the float tables, 1.2e-4 in the quietest (LP24 into HP24 at
3 kHz, little left but the rounding). The frame cache and the direct reads agree within 2.3e-6;
the output stage matches the old one within 4.7e-7 (summation order). `test/m3_test.cpp` plays
still, gliding and over-full cases with the frame cache on and off (`Synth::setFrameCache`).

## Fourth pass: filter step, bus transpose, unison stacks

A profile by function and source line under `qemu-arm` (a TCG plugin counting each translated
block's executions) showed where the instructions went. At 8 voices × 8 unison the float wavetable
reader was 57% of the block; at 8 voices × 1, the filters with their set-up were 40%.

- **The SVF step rearranged.** GCC turned `2 v1 - ic1` into a multiply-add on `-ic1`, which costs
  a negation and a register move per state per sample on NEON, and spills in the 24 dB types. The
  step now computes the new ic1 as `b1 ic1 + b2 v3` (b1 = 2 a1 − 1, b2 = 2 a2). The new ic2 is
  `v2 + t`, with t = a2 ic1 + a3 v3. It is the same filter, and the band output is only computed
  for the types that use it. LP24's loop went from 30 to about 19 instructions per four voices'
  sample.
- **Sources into the buses by transposing.** Each lane renders into its own contiguous buses.
  Then four lanes by four samples at a time are transposed into the filters' layout
  (`gatherBuses`). This replaces a scalar store per sample and the clearing of the buses. A serial
  patch with nothing else routed to filter 2 now filters F1's bus in place instead of copying it.
- **One call per unison stack.** The wavetable reader takes a whole stack. Its set-up used to cost
  about 70 instructions per unison voice and chunk.
  - It is compiled per level length, so the shifts are immediates, which frees a register in the
    16-bit morph's loop.
  - The interpolation fraction is the phase's fraction bits shifted to the top and converted as a
    fixed-point number: 2 instructions instead of 3.
  - The float pairs load straight into the halves of two q registers, in a five-instruction `asm`
    block. Around a `vcombine`, GCC moved registers five times per four samples.
- **Two unison voices per pass** on the frame cache's float frames. Each voice's samples are
  multiplied by its own gains and summed, then by the stack's gliding level, so there is one load
  and one store of the output per pair. The 16-bit readers have no registers to spare for this.
- **Drive** is computed as `dry · x + sat · softclip(x · pre)`, with the reciprocal refined once
  (about 16 bits). The drive stage is memoryless, so this is enough there. The Dirty engine's
  saturator inside the filter loop keeps the full reciprocal, because there the error would add up
  as damping.

ARM instructions per block, before and after: the plain `-O3` build, then the profile-guided one
that ships. The patch is the third pass's, with the positions still. "Moving" means an LFO sweeps
both positions. Comb and vowel are in F1.

| Scene | Before | After | PGO before | PGO after |
|---|---|---|---|---|
| 1 voice | 46.3 k | 38.1 k (−18%) | 42.6 k | 35.7 k (−16%) |
| 4 voices | 83.2 k | 71.1 k (−15%) | 76.8 k | 67.1 k (−13%) |
| 8 voices | 161.1 k | 140.2 k (−13%) | 148.9 k | 131.4 k (−12%) |
| 8 voices, moving | 190.5 k | 171.1 k (−10%) | 178.0 k | 158.0 k (−11%) |
| 8 voices, comb | 251.7 k | 237.2 k (−6%) | 236.4 k | 225.6 k (−5%) |
| 8 voices, vowel | 291.4 k | 276.9 k (−5%) | 292.1 k | 281.4 k (−4%) |
| 8 × 4 unison, Dirty | 251.7 k | 206.5 k (−18%) | 246.2 k | 199.1 k (−19%) |
| 8 × 8 unison | 346.3 k | 260.0 k (−25%) | 330.3 k | 250.1 k (−24%) |
| 8 × 8 unison, moving | 473.4 k | 405.9 k (−14%) | 471.1 k | 384.5 k (−18%) |
| 8 × 8 unison, busy matrix | 436.0 k | 358.6 k (−18%) | 429.1 k | 346.2 k (−19%) |

On the Force (2026-10-26, `make bench-device` setup), the build from just before this pass and this
pass's were benched one after the other, pinned to core 1 with MPC running. Both builds are
profile-guided, built with GCC 13. The table gives p99 in % of the block:

| Case | Before | After |
|---|---|---|
| 1 voice | 1.7 | 1.6 |
| 4 voices | 2.6 | 2.4 |
| 8 voices | 4.6 | 4.3 (−7%) |
| 8 × 2 unison | 5.7 | 4.9 (−14%) |
| 8 × 4 unison | 7.7 | 6.4 (−17%) |
| 8 × 8 unison | 11.8 | 9.2 (−22%) |
| 8 × 1, busy matrix | 5.7 | 5.4 (−5%) |
| 8 × 8, busy matrix | 11.2 | 9.3 (−17%) |
| 8 × 8, positions swept, Classic | 12.7 | 9.9 (−22%) |
| 8 × 8, positions swept, 256-frame table | 13.7 | 10.8 (−21%) |

The unison cases gain as much as qemu's counts said, or more, so the `asm` gather and the
transposes hold up on the device. At 8 × 1 the gain is about half of qemu's: −7% against −12%.

The output was compared with the engine before this pass on ARM over 139 scenes. The scenes cover
every filter type, the engines, unison 1–8, the routings, steals, glides, odd block sizes and busy
matrices. All are within 7.8e-6 relative RMS. The drive's reciprocal accounts for that figure;
without it they are within 2.2e-6.

## Memory traffic

A 256-frame table is 4.5 MB, and the Force's L2 cache is about 1 MB. A position that holds still
reads an 8 KB float copy whatever the table's size; a moving one reads the table itself.
`make bench-device WAVETABLES=<folder>` measures whether that waits on memory. It runs a profiling
build (`polywerk_stages.so`) that reports each pass's time per block. Then it plays the same
voices with the positions swept, first on the built-in Classic table and then on a 256-frame table
from that folder.

Measured on 2026-10-26 after the fourth pass, with the positions swept at 8 × 8:

- In the profiling build, the sources pass took 167 µs per block on Classic and 181 µs on the
  256-frame table, 8% more.
- In the shipped build, p99 went from 9.9% to 10.8% of the block. The build from before the pass
  showed the same gap: 12.7% to 13.7%.

So memory costs about one point of the block. It is not what limits the oscillators. Prefetching
the frames a moving position is heading for could win back that point at most.

The profiling build is a plain (not profile-guided) build, so its times read a little higher than
the shipped `.so`'s.

## CPU guard

The plugin adds up the engine's own CPU time (not MPC's callbacks) against the real-time budget,
over windows of at least one 128-frame block, so `process()` sub-blocks and small host blocks are
judged like MPC's.

| Window load | Action |
|---|---|
| Two windows in a row over 40% | Fade out the quietest voice that is only ringing out |
| One window over 65% | Fade out two at once |
| A single window between 40% and 65% (a patch rebuild, a burst of note-ons) | Nothing |

Fades take 3 ms, like a steal. Held and pedal-sustained notes are never touched. The status line
shows `GUARD n` while it acts; `PF_CPU_GUARD=0` turns it off (the test suite does).

## Wavetable import

A 256-frame table first took **582 ms and 22 MB** on the Force. Two changes (Milestone 1):

- **Shorter high mip levels:** level *k* stores `clamp(8 · (1024 >> k), 256, 2048)` samples
  instead of 2048 — 2.4× less memory, the audible levels 0–2 unchanged.
- **Two frames per FFT:** two real frames share one complex FFT, halving the FFT count.

Since the third pass the samples are 16-bit with a scale per frame, quantised as each frame pair
is built. Now about **120 ms and 4.5 MB** on x86 (`-O2`); `make bench-device` times it on the device.
Loading always happens off the audio thread.

## Considered and left out

- **Float copies for moving positions:** the frame cache only pays off for a position that stays
  put; a moving one would need a new copy every chunk, and float tables would double the memory.
- **Voices on a second core:** MPC already runs instances on its audio workers in parallel, and
  handing voices to another thread inside a 2.9 ms block risks dropouts that can't be tested
  without the device.
- **More than 8 voices or 8 unison:** 16 × 16 measured 45% of a block before the NEON pass; 8 × 8
  stays the ceiling until the device bench of the current build says otherwise.
- **Comb and vowel four voices at a time:** they still run one voice at a time, so a vowel filter
  costs twice what LP24 does at 8 voices. Only 6 of the 205 factory presets use one, so this was
  left out of the fourth pass.
- **Table indices from the phase vector** (two `vmov` from NEON to core registers instead of the
  core's own shifts) would take fewer instructions and free core registers in the morph's loop.
  Transfers in that direction stall some ARM cores, so this waits for a device bench.
