# Building and testing

- [Requirements](#requirements)
- [Quick start](#quick-start)
- [Make targets](#make-targets)
- [Make variables](#make-variables)
- [Tests](#tests)
- [Benchmarking on the device](#benchmarking-on-the-device)
- [Packaging and installing](#packaging-and-installing)
- [Binary compatibility](#binary-compatibility)

---

## Requirements

PolyForce builds on Linux or WSL; it is developed on Ubuntu 24.04.

| Tool | Needed for |
|---|---|
| `g++` 13 | tests and the x86 bench |
| `arm-linux-gnueabihf-g++` 13 | the device build (the Force ships GCC 13's libstdc++, linked dynamically) |
| GNU make ≥ 4.3 | everything |
| `python3` | generating the parameter list, layout and C++ headers from `surface/surface.py` |
| `gcc` | the skin generator's C renderer |
| Python 3 with Pillow (`PY=`) | the skin, the page previews and the release package |
| `qemu-user` (`qemu-arm`) | `test-arm`, `test-arm-pgo` and the profile-guided device build |
| `ssh`, `scp` | `bench-device`, `plugin-install` |

On Ubuntu 24.04, for example:

```sh
sudo apt install g++ g++-arm-linux-gnueabihf make python3 python3-pil qemu-user
```

## Quick start

```sh
make test                      # the full suite under ASan/UBSan
make arm-plugin                # build/arm/polyforce.so
make skin preview              # the skin, and every page as surface/build/page_*.png
make plugin-package            # dist/PolyForce-<version>-mpc-armv7.zip
```

`surface/surface.py` is the single source of the parameter list and the touchscreen pages. Every
build regenerates `params.json`, `layout.conf`, `vst.json` and the C++ headers from it when it
changes; it checks the layout and every factory preset before writing anything.

## Make targets

| Target | What it does |
|---|---|
| `surface` | Regenerate parameters, layout and C++ headers from `surface/surface.py` (automatic) |
| `skin` | Build the skin (`TUI.json` + PNGs) with the vendored generator, then polish it with `surface/skin_polish.py` |
| `preview` | Render every page as `surface/build/page_*.png` |
| `test` | The ASan/UBSan suite; uses `$(WAVETABLES)` for the import check |
| `test-arm` | The same suite built for the Force's CPU, run under `qemu-arm` |
| `test-arm-pgo` | The suite linked against the profile-guided objects the shipped `.so` is made of |
| `test-tables` | Load, check and play every WAV under `$(WAVETABLES)`, with load time and memory |
| `bench` | x86 bench: only proves the bench and the profiling build work |
| `arm-plugin` | `build/arm/polyforce.so`; profile-guided when `qemu-arm` is installed |
| `arm-bench-stages` | `build/arm/polyforce_stages.so`, the profiling build (never shipped) |
| `bench-device` | Run the CPU bench on a device (see [below](#benchmarking-on-the-device)) |
| `plugin-package` | `dist/PolyForce-<version>-mpc-armv7.zip` with the installer |
| `plugin-install` | Package, copy to the device and install (stops and restarts MPC) |
| `clean` | Remove `build/` and `surface/build/` |

`make` on its own runs the tests and builds the device `.so` and the x86 profiling build.

## Make variables

| Variable | Meaning |
|---|---|
| `FORCE` | The device's SSH address, `root@<ip>`; required by `bench-device` and `plugin-install` |
| `SSH_KEY` | Private key for the device's root login (default: ssh's own keys and config) |
| `PY` | Python 3 with Pillow, for `skin`, `preview` and `plugin-package` (default `python3`) |
| `WAVETABLES` | Folder of sample wavetables for the tests and the bench (default `../wavetables`) |
| `PGO` | `auto` (default): profile-guided when `qemu-arm` is installed; `1`: always; `0`: plain build |
| `PLUGIN_VERSION` | Version in the package name |
| `BENCH_ARGS`, `BENCH_MATRIX_ARGS`, `BENCH_STAGE_ARGS` | `pfbench` arguments for the three `bench-device` runs |
| `TABLE` | The large table for the bench (default: the first WAV of 2 MB or more under `WAVETABLES`) |

Pass variables on the command line, or keep your own in `local.mk` next to the Makefile (git
ignores it):

```make
FORCE      = root@192.168.0.10
SSH_KEY    = $(HOME)/.ssh/force
PY         = $(HOME)/.venvs/polyforce/bin/python
WAVETABLES = $(HOME)/wavetables
```

## Tests

`make test` drives the whole plugin through its VST2 entry points against a fake MPC host
(`test/host.h`), under AddressSanitizer and UndefinedBehaviorSanitizer. There is one test file per
milestone (`test/m1_test.cpp` … `m7_test.cpp`), `plugin_test.cpp` for the plugin glue, and
`review_test.cpp` with a regression check for every finding of the code reviews.

`make test-arm` runs the same suite cross-compiled for the Force's CPU under `qemu-arm` (no
sanitizers). It catches 32-bit and ARM-only paths: the denormal flush, NEON vectorisation.

### Sample wavetables

The import check and `make test-tables` need a folder of Serum-format WAVs. Sample tables are
third-party content: keep them **outside** the repository, never commit or package them. Without
them the import check is skipped. Point `WAVETABLES` at your folder if it isn't `../wavetables`:

```sh
make test WAVETABLES=~/wavetables
```

### Environment overrides

| Variable | Replaces |
|---|---|
| `PF_TABLE_ROOTS` | The wavetable roots (colon-separated list) |
| `PF_PRESET_ROOTS` | The preset roots |
| `PF_TUNING_ROOTS` | The tuning roots |
| `PF_DATA_DIR` | Where favorites and recent lists are kept |
| `PF_CPU_GUARD=0` | Turns the CPU guard off (the test suite does) |
| `PF_WAVETABLES` | The folder the import check reads (`make test` sets it from `WAVETABLES`) |

## Benchmarking on the device

```sh
make bench-device FORCE=root@<ip> WAVETABLES=<folder>
```

Copies the plugin, its profiling build, the bench (`pfbench`) and one large table (the first of
2 MB or more under `WAVETABLES`) to `/tmp` on the device, runs pinned to core 1 while MPC keeps
running (MPC's audio workers own cores 2–3), then deletes them. The bench `dlopen()`s the `.so` like
MPC and times every block. It reads no user folders, saves nothing, and fails if any run fails.

Three runs:

1. the plain `.so` at 1–8 voices × 1–8 unison: the verdicts (PASS at p99 ≤ 15% of the block)
2. the same with a busy modulation matrix
3. the profiling build: time per render pass, then the same voices on the built-in table and on
   the large one, to show whether memory traffic matters

Results and what they mean: [Performance](PERFORMANCE.md).

## Packaging and installing

```sh
make plugin-package
```

Builds `dist/PolyForce-<version>-mpc-armv7.zip`: the plugin and its skin as one folder, the
installer and uninstaller, a generated `INSTALL.md` and checksums. Shipped scripts run under
BusyBox on the device, so the build refuses CRLF line endings in them.

```sh
make plugin-install FORCE=root@<ip>
```

Packages, copies the package to the device and runs its installer: it **stops MPC** (save your
project first), backs up and edits `MPC.settings`, and starts MPC again. A reinstall keeps the
user's wavetables, presets, tunings and favorites/recent lists.

## Binary compatibility

- The `.so` exports only `VSTPluginMain` and links with `--no-undefined`: an unresolved symbol
  would otherwise only show as MPC crashing on load.
- It needs glibc 2.38 (`__isoc23_strtol`), which is too new for devices still on MPC OS 2.x. The
  device build prints the highest glibc version it needs.
