# Milestone 1 design: wavetable library, browsing, steal modes

Status: **built** (Milestone 1, 2026-10-04); kept as the design record. Decisions behind it are
in [ROADMAP.md](ROADMAP.md#decisions): all browsing options A–F, tables from the plugin folder
**and** the SSD, all four steal modes. Every touchscreen mechanic below is copied from
RackForcePlugin (`plugin/surface.{h,cpp}`), where it is device-proven.

What changed after this design:
- §5 Quietest is written (released voices first, then the quietest held one), and stealing
  fades the old voice out over 3 ms (Milestone 2).
- §6.2 `br_osc` became `br_target` with a third option, PRESETS: the same page browses presets
  (Milestone 7). Presets and tunings use the same library and loader code (`FileLibrary`,
  `Loader::SlotType`) with their own roots and favorites/recent files.
- §8 the state is now `polyforce 4` (real values, per-oscillator routes, tuning and preset keys);
  see `plugin/state.h`.
- §1 the plugin folder comes from our own `plugin/paths.cpp` (`/proc/self/maps`), not the
  toolkit's wrapper header.
- §3 the loader has three slots (osc 1, osc 2, tuning); it also frees replaced tables while no
  block runs, and catches anything a load throws. MISSING shows in the status line for 5 s, then
  only in the stepper text.
- Still to do on the device: step 7 (all 379 tables reachable, CPU, project reload) and the
  `setParameter`-after-`effSetChunk` trace from §8.

Build order (each step ends green on `make test`; steps 1 and 7 also run `make bench-device`):

1. Faster, smaller import (§4)
2. Library (§1) + favorites/recent files (§7)
3. Cache + per-instance loader (§2, §3)
4. Table steppers on the OSC page + state recall (§6.1, §8)
5. Browser page (§6.2) + favorite/random/copy/swap (§7)
6. Steal modes (§5)
7. Device check: all 379 tables reachable, CPU ≤ 15% at 8 × 8, project reload restores tables

---

## 1. Library (process-wide, UI + worker threads only)

New `plugin/library.{h,cpp}`. Built once per process on first use; the audio thread never sees it.

- **Roots**, in this order:
  1. `<plugin dir>/Wavetables`, plugin dir from `third_party/.../wrapper/plugin_dir.h`
     (`mpc_plugin_dir()` reads `/proc/self/maps`; it deliberately avoids `dladdr()`, which
     would bind GLIBC_2.34).
  2. `/media/AkaiForce/Wavetables` (the SSD; `noexec` only stops binaries, WAVs are fine).
     Missing or unmounted → silently no tables from it.
  - Test override: `PF_TABLE_ROOTS` = colon-separated list replacing both.
- **Scan** = directory listing only (no WAV headers, no FFTs): `*.wav`, case-insensitive,
  recursive. Few ms for ~400 files.
- **Key** (what state and favorites store): `plugin:<relpath>`, `ssd:<relpath>`,
  `builtin:<Name>`. Never an index (indices move when files are added).
- **Category** = first path component under the root (files directly in a root: `Unsorted`).
  Same-named categories from both roots merge. Order: `★ Favorites`, `Recent`, `Built-in`,
  then folders A→Z. Inside a category: by display name, case-insensitive.
- **Display name** = file stem minus the longest prefix ending in `" - "` that every file in
  the category shares: `ESW Analog - Jupiter 8 Saw` → `Jupiter 8 Saw`. Stepper text =
  `Category / Name`.
- **Flat order** (what the knob scroll walks) = categories in order, pseudo-categories
  excluded, built-ins first.
- Guard everything with one mutex; it's touched only from MPC's UI thread and loader threads.

## 2. Cache (process-wide)

- `std::map<key, std::shared_ptr<const Wavetable>>` + LRU list + byte count, one mutex.
- `get(key)` → hit moves it to the front. `put(key, table)` then evicts from the back while
  over the cap (**96 MB**), skipping entries with `use_count() > 1` (someone plays them).
- The same table on both oscillators or in two instances is loaded once.
- Two instances missing the same key at once may both load it; the second `put` just keeps
  the first copy. Acceptable.
- Built-in tables are wrapped in a `shared_ptr` with a no-op deleter (static lifetime).

## 3. Per-instance loader and the audio-thread handoff

New `plugin/loader.{h,cpp}`, one per plugin instance, its own worker thread (started in
`VSTPluginMain`, joined in `effClose`, like RackForce's worker). Two slots, one per oscillator.

```
slot (per oscillator)
  UI thread   wantKey + wantTime (mutex)          what the user picked, when
  worker      owned  : shared_ptr<const Wavetable>   the instance's reference
              loadedKey, state (Ready/Loading/Missing), frames (atomic, for display)
  audio       live   : std::atomic<const Wavetable*>
instance      epoch  : std::atomic<uint32_t>        incremented at the END of every block
              graveyard: vector<{shared_ptr, epochAtSwap}>  (worker only)
```

- **Debounce:** the worker loads a slot only once `wantKey` has been stable for **150 ms**,
  so scrolling through 50 tables loads one.
- **Load:** cache hit or `loadWavetable()` + `put`. Failure or missing file → fall back to
  `builtin:Classic`, state Missing; **`wantKey` is kept**, so saving the project keeps the
  reference and it comes back when the file does.
- **Publish:** `old = owned; owned = new; live.store(new.get()); graveyard.push({old, epoch.load()})`.
- **Free:** the worker drops a graveyard entry once `epoch > epochAtSwap`.
  Why that is safe: the audio thread reads `live` once at block start and uses only that
  pointer until the block ends, then increments `epoch`. If it read the old pointer before the
  swap, that block hasn't ended when the worker samples `epoch`, so freeing waits for its end.
  If it read after the swap, it has the new pointer. Without blocks running (suspended),
  entries wait; `effClose` frees everything after the thread is joined.
- **Audio thread:** at block start `patch.osc[o].table = slot[o].live.load(acquire)` before
  `setPatch` (Synth uses the pointer only inside `render`), `epoch.fetch_add(1)` after the block.
  No locks, no frees, no allocation.
- Status line while busy: `LOADING <name>`; on failure `MISSING <name>` for a few seconds.

## 4. Faster, smaller import (do first)

Measured: a 256-frame table took **582 ms and 22 MB** on the Force. Targets: < 150 ms, < 10 MB.

### 4.1 Shorter high mip levels
Level k holds H_k = 1024 >> k harmonics. Linear interpolation needs ~8 samples per cycle of the
top harmonic to stay clean, so store level k at N_k = clamp(8 · H_k, 256, 2048):

| level k | 0 | 1 | 2 | 3 | 4 | 5..10 |
|---|---|---|---|---|---|---|
| harmonics | 1024 | 512 | 256 | 128 | 64 | 32..1 |
| samples | 2048 | 2048 | 2048 | 1024 | 512 | 256 |

9,216 samples per frame instead of 22,528: **2.4× less memory** (22 → ~9.2 MB) and ~2.6× less
inverse-FFT work. Levels 0–2 are unchanged (no quality change where it is audible).

Code changes: `Wavetable` gets per-level offsets and bit widths (`kMipBits[k]`,
`kMipOffset[k]`, frame stride = Σ(N_k + 1)); `Synth::renderOsc` uses
`idx = ph >> (32 - bits)`, `frac = (ph & mask) * 2^-(32 - bits)` with that level's `bits`;
tests check the guard sample at each level's own length.

### 4.2 Two frames per FFT
Two real signals fit one complex FFT. Forward: put frame a in the real part and frame b in the
imaginary part; then A[h] = (X[h] + conj X[N−h]) / 2, B[h] = (X[h] − conj X[N−h]) / 2i.
Inverse: X = S_a + i·S_b → `ifft(X)` = a + i·b. Halves the FFT count for import and for the
built-ins. Process frames in pairs (an odd last frame pairs with silence).

Together about 5× → **~115 ms** expected on the Force. Measure before/after with
`make bench-device` (it times `pfbench -t` on a 256-frame table).

## 5. Steal modes

New enum parameter `steal`, options `Oldest · Quietest · Keep low · Keep high`, default Oldest.
`Patch::steal`; `Synth::victim()` switches on it. Only voices below the `Voices` limit count.

| Mode | Picks |
|---|---|
| Oldest | the voice started first (`age`, wrap-safe `int32_t(a - b) < 0`) |
| Quietest | **TODO(you)**: released voices (`!gate && !sustained`) first, quietest `env[0].v`; else the quietest held voice |
| Keep low | oldest among all except the lowest-sounding note |
| Keep high | oldest among all except the highest-sounding note |

The Quietest scoring (released vs level vs age) is the piece left for the user; it falls
back to Oldest until written. UI: an `enum_v` "STEAL" in the ENV page's OUTPUT frame
(VOLUME and VOICES knobs side by side at cx ≈ 1013 / 1168, the enum below).
Tests (on `Synth` directly): Oldest steals the first note; Keep low/high keep a held low/high
note through 9 more notes; Quietest steals a released voice before any held one.
Later (Milestone 2): a ~3 ms fade before a stolen voice restarts.

## 6. Touchscreen

### 6.1 Table steppers (option A), OSC page
- `o1_wave`/`o2_wave` enums go; in their place (cy = frame top + 44, cx ≈ 740, w ≈ 700, h = 44):
  `stepper key=o1_table` with params `o1_table` (0..1023, `display:"string"`, `type:"stepper"`,
  not automatable) and momentaries `o1_table_prev` / `o1_table_next`. Same for osc 2.
- Text: `Analog / Jupiter 8 Saw` (`LOADING …` while it loads).
- **Stepping = one table per event**, whatever MPC's delta (Q-Link detent 1/128, wheel click
  0.01, touch drag ≈ 0.04, fast spins send 1–3 detents): RackForce's `stepItem()` rule.
  `|n − ours| ≤ kQuant` (0.0015, MPC rounds to 1/1000) = our own value echoed back, ignore;
  otherwise move ±1 in the direction of travel, clamped to the flat list. Constants from
  RackForce: `kQuant 0.0015`, `kGestureMs 300`, `kFirstMoveMax 0.16`.
- Position knob display becomes `FRAME 37 / 256` (`dynamic_display`; frame count from the slot).

### 6.2 Browser page `TABLES` (option B)
Layout (1280 × 800 layout coordinates, plugin area y = 86..714):

| Element | Widget / params |
|---|---|
| status line | existing readout, top |
| target oscillator | `enum_h key=br_osc` options `OSC 1, OSC 2` |
| categories | `list cols=2 rows=8 key=cat` → `cat_1..cat_16`, options `["-","On"]`, `dynamic_display`; `cat_prev`/`cat_next` momentaries if > 16 categories |
| tables | `list cols=3 rows=8 key=tbl` → `tbl_1..tbl_24`, same kind; `tbl_prev`/`tbl_next` momentaries; readout `tbl_page` "PAGE 2 / 4" |
| now loaded | readout `br_now`: `OSC 1: Analog / Jupiter 8 Saw · 256 frames` |
| actions | `toggle key=fav` (lit = current table is a favorite), buttons `rnd` (RANDOM), `copy` (COPY 1 > 2), `swap` (SWAP 1 <> 2) |

- A `list` tile shows its param's **display text** (Value label) and is lit when its value is 1.
  We set the loaded table's tile to 1 and the others to 0, and push changes ourselves.
- Tap = load into the target oscillator (debounce still applies, 150 ms).
- **Bounce rule:** MPC sends a second toggle ~0.7 s after a tap; ignore a revert within 1 s
  (RackForce `toggleBounce`).
- The browser follows the target oscillator: when its table changes (stepper, random, a project
  load), select that table's category and page.
- Q-Links on this page: `o1_table, o2_table, o1_pos, o2_pos, f1_cut, f2_cut, …` so a knob scrolls
  and auditions while the page is open.

### 6.3 Push mechanics (port of RackForce's `Surface::notify`)
- Arrays `want_[NPARAMS]` (what MPC should show) / `shown_[NPARAMS]` (what it last saw),
  `release_[]` for momentaries (spring back to 0), `textGen_` bumped whenever any dynamic text
  changes.
- From `processReplacing` only: `audioMasterAutomate` for changed values, at most **48 per
  block** (round-robin cursor); `audioMasterUpdateDisplay` at most every **4 blocks** when
  `textGen_` moved. **Never call the host from `setParameter` or the dispatcher.**
- MPC doesn't notice engine-side value changes (tile lit states, stepper position) unless
  they're automated, and doesn't re-read names/texts without UpdateDisplay.
- Momentaries fire on the rising edge only (`held_[]`).
- Parameter count after this milestone: ~101 (44 − 2 + 59). No MPC limit is known (a catalog
  plugin declares 669).

## 7. Favorites, Recent, Random, Copy, Swap

- **Favorites:** a process-wide set of keys, saved to `<plugin dir>/favorites.txt` (one key per
  line) on every toggle; read at library build. Shared by every project. Test override:
  `PF_DATA_DIR`.
- **Recent:** the last 12 loaded keys, newest first, in `recent.txt` next to it.
- **Random:** a random table from the browser's current category (pseudo-categories included),
  never the one already loaded, into the target oscillator.
- **Copy 1 > 2:** osc 2 gets osc 1's key (cache hit: instant). **Swap:** exchange the keys.

## 8. State

- Chunk (`polyforce 2`) gains `o1_table=<key>` and `o2_table=<key>`: string values, the only
  non-numeric lines; written from `wantKey`. Missing lines (older states) → `builtin:Classic`.
- `steal=<index>` like any enum.
- The `o1_table` param value (an index) is derived from the key after every library change,
  never saved.
- Risk to check with a trace on the device: if MPC calls `setParameter` with stored values
  right after `effSetChunk`, the relative stepping in §6.1 must see them as echoes. They equal
  our read-back, so they should be; confirm.

## 9. Tests to add

- Library: with `PF_TABLE_ROOTS=../wavetables`, categories = Built-in + the 11 ESW folders;
  `ESW Analog - ` prefix stripped; flat order stable; keys round-trip to paths.
- Loader: stepper next → `LOADING` → loaded (poll with timeout); a project chunk with
  `o1_table=` restores in a fresh instance; a missing key → Classic + `MISSING`, key kept in
  the saved state; scrolling 30 steps fast loads only the last table (count loads).
- Handoff: under ASan, swap tables while blocks render on another thread for a few seconds
  (catches use-after-free).
- Browser: category tap fills tiles; table tap loads into the target; lit tile pushed via
  `audioMasterAutomate` (the fake host records it); bounce within 1 s ignored.
- Favorites/Recent files written under `PF_DATA_DIR`; random never repeats the current table;
  copy/swap.
- Steal modes as in §5.
- Import: `test-tables` still all 379 OK; loads at least 3× faster than today on x86
  (334 ms average before).
