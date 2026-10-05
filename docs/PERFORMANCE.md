# Performance

- [The budget](#the-budget)
- [Device measurements](#device-measurements)
- [First pass: NEON](#first-pass-neon)
- [Second pass: control rate, matrix, PGO](#second-pass-control-rate-matrix-pgo)
- [Open question: memory traffic](#open-question-memory-traffic)
- [CPU guard](#cpu-guard)
- [Wavetable import](#wavetable-import)
- [Considered and left out](#considered-and-left-out)

---

## The budget

MPC renders 128-frame blocks: **2902 µs per block**, per plugin instance. A plugin passes at
**p99 ≤ 15%** of the block (warns up to 35%). PolyForce's ceiling, **8 voices × 8 unison** with
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

By voice count, v0.0.2:

| Voices | × 1 unison | × 4 | × 8 |
|---|---|---|---|
| 4 | 4.3 | 5.8 | 7.8 |
| 8 | 8.2 | 11.4 | 15.2 |

Both passes below came after these measurements; the current build is still to be measured on the
device.

Other Phase 0 numbers: `VSTPluginMain` (builds the four built-in tables) takes 163 ms on the Force.
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

## Open question: memory traffic

At 8 × 8 the oscillators are now about 70% of the block. A 256-frame table is 9 MB; the Force's L2
cache is about 1 MB. Whether the oscillators wait on memory is what the stage bench answers on the
device: `make bench-device WAVETABLES=<folder>` runs a profiling build (`polyforce_stages.so`) that
reports each pass's time per block, then plays the same voices with the positions swept on the
built-in Classic table and on a 256-frame table from that folder.

- If the sources pass grows clearly with the large table, **16-bit tables** (half the memory
  traffic) are the next step.
- If not, they would only cost precision.

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

Now about **120 ms and 9.0 MB** on x86 (`-O2`); `make bench-device` times it on the device.
Loading always happens off the audio thread.

## Considered and left out

- **Precomputed float frame pairs** for the oscillator reads: about one instruction in eleven saved
  for twice the table memory, while the open question is memory traffic.
- **Voices on a second core:** MPC already runs instances on its audio workers in parallel, and
  handing voices to another thread inside a 2.9 ms block risks dropouts that can't be tested
  without the device.
- **More than 8 voices or 8 unison:** 16 × 16 measured 45% of a block before the NEON pass; 8 × 8
  stays the ceiling until the device bench of the current build says otherwise.
