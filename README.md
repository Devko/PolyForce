# PolyForce

A wavetable synth that runs **inside MPC on the Akai Force** as a native VST2 instrument,
modelled on the feature set of u-he Hive 2 (our own name, DSP, tables and presets; nothing of
u-he's is used). Working name; the plugin uid `PlFc` and `polyforce.so` may still change before
the first release.

- Roadmap and decisions: [docs/ROADMAP.md](docs/ROADMAP.md)
- Next milestone, designed and ready to build: [docs/M1_DESIGN.md](docs/M1_DESIGN.md)
- How MPC hosts plugins (ABI, skins, threads, device facts):
  `../RackForcePlugin/docs/MPC_PLUGIN_SPEC.md`

## Status (2026-10-04): Phase 0 spike done, v0.0.2

- 8 voices · 2 wavetable oscillators · unison up to 8 (detune, stereo width) · 4 built-in tables
- 2 filters (Off, LP12, LP24, BP, HP12, HP24, Notch, Peak), resonance, env amount, keytrack,
  drive, serial or parallel
- Amp envelope (with velocity) + mod envelope (→ filter cutoff, → wavetable position)
- Serum-format WAV import (`loadWavetable`), not yet reachable from the touchscreen
- Status line with live voice count and CPU use; pitch bend ±2, sustain pedal, CC 120/123
- Three touchscreen pages (OSC, FILTER, ENV) with Q-Links
- Played on the device by the user (v0.0.1); v0.0.2 (8 × 8 limits, real-value state) built,
  install pending

CPU on the Force (p99, % of the 2.9 ms block, 2 oscillators, LP24+drive → LP12):

| Voices | ×1 unison | ×4 | ×8 |
|---|---|---|---|
| 4 | 4.3 | 5.8 | 7.8 |
| 8 | 8.2 | 11.4 | 15.2 |

## Layout

```
surface/surface.py     THE source of the parameter list and the touchscreen pages: writes
                       params.json, layout.conf, vst.json and build/param_ids.h
                       (ids, value curves, MAX_VOICES / MAX_UNISON)
dsp/wavetable.*        band-limited tables: 11 mip levels x 2048 samples per frame, FFT-built;
                       4 built-ins; Serum WAV loader (one gain for the whole table)
dsp/synth.*            the engine: voices, oscillators (uint32 phase), Simper SVF filters,
                       one-pole ADSRs, 16-sample control rate. No allocation on the audio thread.
plugin/plugin.cpp      VST2 glue: atomics for params, MIDI with sample offsets, chunk state,
                       denormal flush, CPU meter
plugin/patch_map.*     0..1 <-> real values, display text, params -> Patch
plugin/vst2.h          hand-written VST2 ABI slice (from RackForcePlugin)
test/plugin_test.cpp   the whole plugin through its VST2 entry points, ASan/UBSan (266 checks)
test/tables_sweep.cpp  every WAV in a folder: load, check, play, timing and memory
tools/bench.cpp        CPU bench: dlopen()s the .so like MPC, times every block
third_party/mpc-vst-plugins/   sd88me's MIT skin generator + installer (4 marked RackForce patches)
```

## Build and test (WSL, Ubuntu 24.04)

Needs g++ 13, `arm-linux-gnueabihf-g++` 13, GNU make ≥ 4.3, and Python with Pillow at
`~/.venvs/rackforce/bin/python` (shared with RackForcePlugin).

```bash
wsl -e make -C /mnt/d/DEV/mockba/PolyForce test
```

| Target | What it does |
|---|---|
| `surface` | regenerate params/skin from `surface/surface.py` (automatic when it changes) |
| `preview` | render the skin pages to `surface/build/page_*.png` |
| `test` | ASan/UBSan suite; uses `$(WAVETABLES)` (default `../wavetables`) for the import check |
| `test-tables` | load + play every WAV under `$(WAVETABLES)` (379 ESW tables: all pass) |
| `bench` | x86 bench, only proves the bench works |
| `arm-plugin` | `build/arm/polyforce.so` for the Force |
| `bench-device FORCE=root@<ip>` | copies .so + bench + one 256-frame table to `/tmp`, runs on core 1, deletes them |
| `plugin-package` | `dist/PolyForce-<ver>-mpc-armv7.zip` with sd88me's installer |
| `plugin-install FORCE=root@<ip>` | **run by the user**: stops MPC, edits `MPC.settings`, restarts MPC |

The Force's address is DHCP (it was `192.168.1.133` on 2026-10-04). Install:

```bash
wsl -e make -C /mnt/d/DEV/mockba/PolyForce plugin-install FORCE=root@192.168.1.133
```

## Rules worth knowing

- **Parameters:** free to change until v0.1, then append-only (MPC projects store values by
  index). Saved state is `polyforce 2` = key=value lines of *real* values, so range changes
  don't remap saved projects; `polyforce 1` (0..1 values) is still read.
- **Limits:** `kMaxVoices`/`kMaxUnison` in `dsp/synth.h` must equal `MAX_VOICES`/`MAX_UNISON` in
  `surface.py` (a static_assert in `patch_map.cpp` enforces it).
- **Real time:** nothing on the audio thread allocates, locks or throws; host callbacks only
  from `processReplacing`; a `try/catch` stands between every entry point and MPC.
- **The sample tables** in `D:\DEV\mockba\wavetables` (Echo Sound Works Core Tables, 379 WAVs,
  third-party) are test data: never committed, never packaged.
- The `.so` needs GLIBC_2.38 (`__isoc23_strtol`), the same as RackForce: fine on this Force,
  too new for MPC OS 2.x devices.
- The repo is **not under git yet**.

## Open for you

`Synth::victim()` in [dsp/synth.cpp](dsp/synth.cpp) (marked `TODO(you)`): the voice-stealing
policy. Milestone 1 turns it into a Steal selector; see [docs/M1_DESIGN.md](docs/M1_DESIGN.md) §5.
