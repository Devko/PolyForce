# Interface

How the touchscreen pages are designed and built. For what's on each page, see the
[user guide](USER_GUIDE.md#the-screen).

- [What MPC skins can do](#what-mpc-skins-can-do)
- [Design](#design)
- [Wave view](#wave-view)
- [Building the skin](#building-the-skin)

---

## What MPC skins can do

MPC draws a plugin's screen from a skin: static PNGs plus a JSON layout. That decides most of the
design.

| Fact | Consequence |
|---|---|
| Static images only: no drawn lines, no text entry | No scopes or drag-and-drop; text, knobs and tiles. User presets are numbered |
| `list` tiles and `stepper` texts can change at runtime | File names, folders and frame numbers can be shown |
| Popup option texts are baked into PNGs | Popups only for fixed lists (filter types, mod sources, steal modes), never for files |
| MPC shows a parameter's own name under its control and in the Q-Link overlay | Names are short and unique |
| Data wheel = 0.01 per click, Q-Link = 1/128 per detent | Stepped parameters snap to exactly one step per event |

## Design

The pages are defined in `surface/surface.py` (`pages()`).

- **Look:** rounded cards (`style=td3`) on one flat ground (`#15181d`, the same colour as the card
  fill, so no control shows a box behind it), a teal accent (`#3fd0c0`), Titillium Web.
- **Structure:** seven tabs in MPC's tab strip: OSC, FILTER, MOD, MATRIX, BROWSE, VOICE, SEQ (BROWSE
  among the first five). A tab with several pages uses MPC's own sub-pages, as AIR DrumSynth Multi
  does: dots under the tab, a tap on it again for the next page, each page its own screen. Every page
  has a header row (the status line; FILTER's routing and engine and BROWSE's target beside it) over
  cards in two rows of 270 px or one of 552 px.
- **Pages, not panels:** an earlier build stacked a tab's pages as panels on one screen with a page
  selector, and used the sub-pages only for more Q-Link sets. On the Force that read as sub-pages
  that never change, and MPC builds every hidden panel too (the matrix, 1822 components, took ~0.9 s
  to open). Each page is now a `[tab]` of its own in `layout.conf`; `skin_polish.py` numbers them as
  sub-pages from `skin_style.json`'s `tab_groups`.
- **Knobs** have a value arc from the minimum, or from 12 o'clock for bipolar parameters (pan, fine
  tune, amounts…). The generator draws one filmstrip per knob radius, so the radius picks the look
  (30, bipolar 29; small 22, bipolar 21); `KNOB_STYLES` in `surface.py` is the one place for it.
- **Names:** MPC shows a parameter's own name under its knob or slider and in its Q-Link overlay
  (not the layout's `label=`), so names are at most 13 characters for knobs, sliders and toggles,
  unique, and must fit their label.
- **Q-Link sets:** one per page, titled like the page (MPC shows it in the tab strip, at most 12
  characters).
- **Checks:** before writing anything, `surface.py` checks the layout with the generator's own
  sizes (knob, slider and button boxes, enum labels, open popup lists), keeps controls and text out
  of the card title bands, and limits bitmap text to the glyphs its font has.

## Wave view

OSC → WAVES shows each oscillator's current frame (its table at the position knob, morphed between
frames like the oscillator) as 48 bars on a dark panel, with the table stepper, position and level
beside it.

Skins can't draw lines, so each bar is a display-only filmstrip (the generator's `meter`, 128
heights) bound to a parameter the plugin sets: the sample of largest magnitude in that 48th of the
cycle, so narrow peaks show.

- The plugin computes and pushes the columns only when the table, wave or position changed, at most
  every 16 blocks (~46 ms). MPC doesn't tell a plugin which page shows, so this runs on every page.
- MPC writing to them (a touch) is undone; they are not saved or automatable.
- It shows the knob's position, not the modulated one.
- The meters need local patch 5 to the generator (`third_party/mpc-vst-plugins/README.md`);
  `skin_polish.py` draws their strip.

## Building the skin

```sh
make skin      # the skin: TUI.json + PNGs
make preview   # every page as surface/build/page_*.png
```

`make skin` runs the vendored generator (`gen_vst.py`), then `surface/skin_polish.py`, which
redraws, keeping every file name and size:

- the knob filmstrips (arc knobs)
- the trigger buttons (rounded, full size, real label; SAVE and AUTO-ASSIGN in the accent)
- the stepper arrows (the generator cuts those of a stepper inside a page mode from the wrong image)
- the wave view's bar strip

and then numbers the tabs as sub-pages of their groups (TUI.json and the Q-Links files).

It first checks the skin against `layout.conf` and `build/skin_style.json` and fails the build on
any mismatch. `python3 surface/skin_polish.py --selftest` runs it on a fabricated skin.
