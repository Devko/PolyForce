#!/usr/bin/env python3
"""PolyForce touchscreen surface: the ONE place the plugin's parameters and pages are defined.

Writes, next to this file:
  params.json        ordered VST parameter list (index = position; append-only once released:
                     MPC projects store values by index)
  layout.conf        the skin (shadow_page.conf syntax, see
                     third_party/mpc-vst-plugins/tools/shadow_skin.py); coords are 1280x800
                     Force-Shadow pixels, the plugin area is y = 86..714
  vst.json           plugin identity for the vendored gen_vst.py
  build/param_ids.h  everything the C++ is compiled against: parameter ids, kinds, value
                     curves, names, options and defaults (the C++ never reads gen_vst's params.h,
                     so `make test` and the .so build need only Python, not the skin toolchain)
  build/skin_style.json  the palette, knob looks and primary buttons for skin_polish.py, which
                     `make skin` runs after the generator

Continuous parameters are declared to MPC as 0..1: the real range and curve (log Hz, log
seconds, ...) live in param_ids.h, and the plugin formats every value text itself, so the
knob, its label and the DSP can never disagree.

Before writing anything the layout is checked the way shadow_skin.py would (unknown keys,
option counts, when=, Q-Link sets) plus geometry with shadow_skin's own sizes (inside the plugin
area, no overlaps within a page or mode panel, nothing in a card's title band, open popup lists
inside the plugin area, bitmap-font glyphs) and the parameter names MPC shows (short, unique), so
a broken page fails here instead of on the device.

Run: python3 surface/surface.py   (make surface does this)
"""
import json
import math
import os
import re
import shlex
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# Polyphony and unison ceilings. They must equal kMaxVoices/kMaxUnison in dsp/synth.h
# (patch_map.cpp static_asserts it). 8 x 8 x 2 oscillators measured 15.5% of a block on
# the Force (2026-10-04); 16 x 16 was 45%.
MAX_VOICES = 8
MAX_UNISON = 8
WAVE_COLS = 48              # the wave view's columns per oscillator (plugin/plugin.cpp fills them)
FILTER_TYPES = ["Off", "LP12", "LP24", "BP", "HP12", "HP24", "Notch", "Peak", "Comb+", "Comb-", "Vowel"]   # dsp FilterType
ENGINES = ["Clean", "Normal", "Dirty"]
ROUTING = ["Serial", "Parallel"]
STEPPER_RANGE = 1023        # a table stepper's VST range: 0..1023 items (stepItem moves 1 per event)
BROWSER_CATS = 16           # category tiles on the browser page (2 x 8)
BROWSER_ITEMS = 24          # item tiles (3 x 8)

VST = {"name": "PolyForce", "vendor": "Devko", "uid": "PlFc", "version": 1000,
       "so": "polyforce.so", "params": "params.json", "layout": "layout.conf"}


# --- parameters ------------------------------------------------------------------------------
# kind:
#   synth    a sound parameter: saved in the state, automatable; curve lin|log|int|pow|enum
#   ui       a stepped choice the surface uses (browser target, seq record): not saved
#   readout  text the plugin writes (status line, "PAGE 2 / 4"): read only
#   stepper  plugin-owned index into a list (tables, presets), text = the item; moves one
#            item per Q-Link/wheel event; comes with <key>_prev / <key>_next buttons
#   button   momentary: acts on the press, springs back to 0
#   tile     a browser tile (list widget): lit = 1, text = the item; a tap acts
#   toggle   plugin-owned on/off (lit state follows the plugin), a tap acts
#   popup    the hidden "<key>__open" flag of a popup list (shadow_skin's popup_params)
#   meter    a value the plugin sets for a display-only filmstrip (the wave view's columns): not saved,
#            not automatable, a host write is undone
# fmt: how the plugin prints the value (see plugin/patch_map.cpp paramDisplay)
P = []


def _add(key, name, kind, curve, lo, hi, default, fmt, **extra):
    d = dict(key=key, name=name, kind=kind, curve=curve, lo=lo, hi=hi, default=default, fmt=fmt)
    d.update(extra)
    P.append(d)


def readout(key, name):
    _add(key, name, "readout", "readout", 0, 0, 0, "none")


def meter_param(key, name):
    _add(key, name, "meter", "lin", 0, 1, 0.5, "none")


def enum(key, name, options, default, ui=False):
    _add(key, name, "ui" if ui else "synth", "enum", 0, len(options) - 1, options.index(default), "enum",
         options=options)


def num(key, name, curve, lo, hi, default, fmt):
    _add(key, name, "synth", curve, lo, hi, default, fmt)


def stepper(key, name):
    _add(key, name, "stepper", "int", 0, STEPPER_RANGE, 0, "text")
    button(key + "_prev", name + " Prev")
    button(key + "_next", name + " Next")


def button(key, name):
    _add(key, name, "button", "int", 0, 1, 0, "none")


def tile(key, name):
    _add(key, name, "tile", "enum", 0, 1, 0, "text", options=["-", "On"])


def toggle(key, name):
    _add(key, name, "toggle", "enum", 0, 1, 0, "enum", options=["Off", "On"])


def popup_flag(of):
    src = next(p for p in P if p["key"] == of)
    _add(of + "__open", "%s List" % src["name"], "popup", "enum", 0, 1, 0, "none", options=["Closed", "Open"],
         popup_of=of)


readout("status", "Status")            # index 0 must stay a read-only readout: MPC sets it at load
num("volume", "Volume", "lin", -60, 6, -6, "db")
num("voices", "Voices", "int", 1, MAX_VOICES, MAX_VOICES, "count")
enum("routing", "Filter Routing", ROUTING, "Serial")

OSC_WAVES = ["Table", "Sine", "Triangle", "Saw", "Square", "Pulse", "Noise"]   # dsp/synth.h OscWave
PHASE_MODES = ["Reset", "Random", "Free"]
ROUTES = ["F1", "F2", "F1+F2", "Direct"]
SUB_WAVES = ["Sine", "Triangle", "Saw", "Square"]

for o, (pos, semi, fine, level) in ((1, (0.66, 0, 0, 0.8)), (2, (0.66, 0, 7, 0.6))):
    enum("o%d_wave" % o, "O%d Wave" % o, OSC_WAVES, "Table")
    popup_flag("o%d_wave" % o)
    stepper("o%d_table" % o, "O%d Table" % o)
    num("o%d_pos" % o, "O%d Position" % o, "lin", 0, 1, pos, "frame%d" % o)
    num("o%d_oct" % o, "O%d Octave" % o, "int", -3, 3, 0, "oct")
    num("o%d_semi" % o, "O%d Semi" % o, "int", -12, 12, semi, "semi")
    num("o%d_fine" % o, "O%d Fine" % o, "lin", -100, 100, fine, "cent")
    num("o%d_uni" % o, "O%d Unison" % o, "int", 1, MAX_UNISON, 1, "count")
    num("o%d_detune" % o, "O%d Detune" % o, "lin", 0, 1, 0.3, "detune")
    num("o%d_width" % o, "O%d Width" % o, "lin", 0, 1, 0.5, "pct")
    num("o%d_level" % o, "O%d Level" % o, "lin", 0, 1, level, "pct")
    num("o%d_pan" % o, "O%d Pan" % o, "lin", -1, 1, 0, "pan")
    num("o%d_phase" % o, "O%d Phase" % o, "lin", 0, 1, 0, "deg")
    enum("o%d_phmode" % o, "O%d Phase Mode" % o, PHASE_MODES, "Reset")
    enum("o%d_route" % o, "O%d Route" % o, ROUTES, "F1")
    enum("o%d_sub_wave" % o, "Sub%d Wave" % o, SUB_WAVES, "Sine")
    num("o%d_sub_tune" % o, "Sub%d Tune" % o, "int", -36, 12, -12, "semi")
    num("o%d_sub_level" % o, "Sub%d Level" % o, "lin", 0, 1, 0, "pct")

num("noise_level", "Noise Level", "lin", 0, 1, 0, "pct")
num("noise_color", "Noise Colour", "lin", -1, 1, 0, "bipct")
enum("noise_route", "Noise Route", ROUTES, "F1")

enum("engine", "Filter Engine", ENGINES, "Normal")
for f, (ftype, cut, env) in ((1, ("LP24", 1200, 0.25)), (2, ("Off", 8000, 0.0))):
    enum("f%d_type" % f, "F%d Type" % f, FILTER_TYPES, ftype)
    popup_flag("f%d_type" % f)
    num("f%d_cut" % f, "F%d Cutoff" % f, "log", 20, 20000, cut, "hz")
    num("f%d_res" % f, "F%d Reso" % f, "lin", 0, 1, 0.25, "pct")
    num("f%d_env" % f, "F%d Env 2" % f, "lin", -1, 1, env, "bipct")
    num("f%d_key" % f, "F%d Keytrack" % f, "lin", 0, 1, 0.5, "pct")
    num("f%d_drive" % f, "F%d Drive" % f, "lin", 0, 1, 0.0, "pct")

for e, (a, d, s, r) in ((1, (0.003, 0.4, 0.8, 0.3)), (2, (0.001, 0.6, 0.2, 0.5))):
    num("e%d_a" % e, "E%d Attack" % e, "log", 0.001, 20, a, "time")
    num("e%d_d" % e, "E%d Decay" % e, "log", 0.001, 20, d, "time")
    num("e%d_s" % e, "E%d Sustain" % e, "lin", 0, 1, s, "pct")
    num("e%d_r" % e, "E%d Release" % e, "log", 0.001, 20, r, "time")
    if e == 1:
        num("e1_vel", "E1 Velocity", "lin", 0, 1, 0.5, "pct")
    else:
        num("e2_pos", "E2 > Pos", "lin", -1, 1, 0.0, "bipct")
        num("e2_vel", "E2 Velocity", "lin", 0, 1, 0.0, "pct")
        enum("e2_loop", "E2 Loop", ["Off", "Loop"], "Off")

# --- modulation (Milestone 5): two LFOs, the 12 x 2 matrix, four XY pads ---
LFO_WAVES = ["Sine", "Triangle", "Saw Up", "Saw Down", "Square", "S&H", "Smooth"]   # dsp/mod.h LfoWave
SYNC_DIVS = ["8 bars", "4 bars", "2 bars", "1 bar", "1/2", "1/2T", "1/4", "1/4T", "1/4.", "1/8", "1/8T", "1/8.",
             "1/16", "1/16T", "1/16.", "1/32", "1/32T"]   # dsp/mod.h kSyncBeats
MOD_SOURCES = ["None", "Env 1", "Env 2", "LFO 1", "LFO 2", "Velocity", "Note", "Mod Wheel", "Aftertouch", "Bend",
               "Random", "Alternate", "Gate", "Seq", "Shape 1", "Shape 2", "Shape 3", "Shape 4",
               "X1", "Y1", "X2", "Y2", "X3", "Y3", "X4", "Y4", "Breath", "Expression", "Constant"]   # ModSource
MOD_TARGETS = ["Off", "Pitch", "Osc1 Pitch", "Osc2 Pitch", "Osc1 Pos", "Osc2 Pos", "Osc1 Level", "Osc2 Level",
               "Osc1 Pan", "Osc2 Pan", "Osc1 Detune", "Osc2 Detune", "Sub1 Level", "Sub2 Level", "Noise Level",
               "Noise Colour", "F1 Cutoff", "F2 Cutoff", "Cutoffs", "F1 Reso", "F2 Reso", "F1 Drive", "F2 Drive",
               "Env1 Attack", "Env1 Decay", "Env1 Sustain", "Env1 Release", "Env2 Attack", "Env2 Decay",
               "Env2 Sustain", "Env2 Release", "LFO1 Rate", "LFO2 Rate", "LFO1 Depth", "LFO2 Depth", "Volume",
               "Pan"]   # ModTarget
MODIFIERS = ["None", "Curve", "Rectify", "Quantize", "S&H", "Slew"]   # ModModifier
MOD_SLOTS = 12

for l in (1, 2):
    p = "l%d_" % l
    enum(p + "wave", "L%d Wave" % l, LFO_WAVES, "Sine")
    popup_flag(p + "wave")
    enum(p + "sync", "L%d Sync" % l, ["Free", "Sync"], "Free")
    num(p + "rate", "L%d Rate" % l, "log", 0.02, 40, 2.0 if l == 1 else 0.5, "lfohz")
    enum(p + "div", "L%d Sync Rate" % l, SYNC_DIVS, "1/4")
    popup_flag(p + "div")
    num(p + "phase", "L%d Phase" % l, "lin", 0, 1, 0, "deg")
    num(p + "delay", "L%d Delay" % l, "pow", 0, 10, 0, "time")
    num(p + "fade", "L%d Fade In" % l, "pow", 0, 10, 0, "time")
    enum(p + "trig", "L%d Trigger" % l, ["Retrig", "Free", "Global"], "Retrig")
    enum(p + "polar", "L%d Polarity" % l, ["Bipolar", "Unipolar"], "Bipolar")
    num(p + "depth", "L%d Depth" % l, "lin", 0, 1, 1, "pct")

for k in range(1, MOD_SLOTS + 1):
    p = "m%d_" % k
    enum(p + "src", "M%d Source" % k, MOD_SOURCES, "None")
    enum(p + "via", "M%d Via" % k, MOD_SOURCES, "None")
    enum(p + "mod", "M%d Modifier" % k, MODIFIERS, "None")
    num(p + "modamt", "M%d Mod Amt" % k, "lin", -1, 1, 0, "bipct")
    enum(p + "t1", "M%d Target 1" % k, MOD_TARGETS, "Off")   # each amount right after its target
    num(p + "a1", "M%d Amt 1" % k, "lin", -1, 1, 0, "modamt")
    enum(p + "t2", "M%d Target 2" % k, MOD_TARGETS, "Off")
    num(p + "a2", "M%d Amt 2" % k, "lin", -1, 1, 0, "modamt")
    for key in ("src", "via", "mod", "t1", "t2"):
        popup_flag(p + key)

# --- sequencing (Milestone 6): arpeggiator, 16-step sequencer, 4 x 8-step shape sequencer ---
ARP_DIRS = ["Up", "Down", "Up/Down", "Down/Up", "Played", "Random", "Chord"]   # dsp/notegen.h ArpDir
SEQ_STEPS = 16
SHAPE_STEPS = 8
enum("seq_mode", "Arp/Seq Mode", ["Off", "Arp", "Seq"], "Off")
enum("arp_dir", "Arp Direction", ARP_DIRS, "Up")
popup_flag("arp_dir")
num("arp_oct", "Arp Octaves", "int", 1, 4, 1, "count")
enum("clk_rate", "Arp/Seq Rate", SYNC_DIVS, "1/16")
popup_flag("clk_rate")
num("clk_gate", "Arp/Seq Gate", "lin", 0.05, 1, 0.5, "pct")
num("clk_swing", "Swing", "lin", 0, 0.5, 0, "pct")
enum("arp_latch", "Arp Latch", ["Off", "Latch"], "Off")
enum("arp_pattern", "Arp Pattern", ["Off", "Steps"], "Off")
num("seq_steps", "Seq Steps", "int", 1, SEQ_STEPS, SEQ_STEPS, "count")
enum("seq_rec", "Seq Record", ["Off", "Rec"], "Off", ui=True)
for k in range(1, SEQ_STEPS + 1):
    num("s%d_note" % k, "Note %d" % k, "int", -24, 24, 0, "semi")
for k in range(1, SEQ_STEPS + 1):
    num("s%d_vel" % k, "Vel %d" % k, "int", 0, 127, 100, "count")
for k in range(1, SEQ_STEPS + 1):
    num("s%d_mod" % k, "Mod %d" % k, "lin", -1, 1, 0, "bipct")
enum("sh_rate", "Shape Rate", SYNC_DIVS, "1/8")
popup_flag("sh_rate")
num("sh_steps", "Shape Steps", "int", 1, SHAPE_STEPS, SHAPE_STEPS, "count")
for l in range(1, 5):
    enum("sh%d_mode" % l, "Shape %d Mode" % l, ["Step", "Ramp", "Smooth"], "Step")
    for k in range(1, SHAPE_STEPS + 1):
        num("sh%d_%d" % (l, k), "Sh%d Step %d" % (l, k), "lin", -1, 1, 0, "bipct")

for x in range(1, 5):
    num("xy%d_x" % x, "XY Pad X%d" % x, "lin", 0, 1, 0, "pct")
    num("xy%d_y" % x, "XY Pad Y%d" % x, "lin", 0, 1, 0, "pct")
button("xy_auto", "XY Auto-Assign")

# --- voices (Milestone 2) ---
enum("vmode", "Voice Mode", ["Poly", "Duo", "Mono", "Legato"], "Poly")
enum("steal", "Voice Steal", ["Oldest", "Quietest", "Keep low", "Keep high"], "Oldest")
enum("same_note", "Same Note", ["Retrigger", "New voice"], "Retrigger")
enum("glide_mode", "Glide Mode", ["Off", "Always", "Legato"], "Off")
enum("glide_type", "Glide Type", ["Time", "Rate"], "Time")
num("glide", "Glide Time", "log", 0.001, 10, 0.08, "time")
num("bend_up", "Bend Up", "int", 0, 24, 2, "semi")
num("bend_dn", "Bend Down", "int", 0, 24, 2, "semi")
num("vel_curve", "Vel Curve", "lin", -1, 1, 0, "bipct")

# --- tuning and presets (Milestone 7) ---
stepper("tuning", "Tuning")
stepper("preset", "Preset")
button("pre_save", "Save Preset")
button("pre_init", "Init Patch")
button("pre_rand", "Randomize")
num("rand_amt", "Rand Amount", "lin", 0, 1, 0.5, "pct")

# --- the browser: wavetables (docs/M1_DESIGN.md §6.2) and presets ---
enum("br_target", "Browse For", ["OSC 1", "OSC 2", "PRESETS"], "OSC 1", ui=True)
for i in range(1, BROWSER_CATS + 1):
    tile("cat_%d" % i, "Category %d" % i)
button("cat_prev", "Categories Prev")
button("cat_next", "Categories Next")
for i in range(1, BROWSER_ITEMS + 1):
    tile("tbl_%d" % i, "Item %d" % i)
button("tbl_prev", "Items Prev")
button("tbl_next", "Items Next")
readout("tbl_page", "Items Page")
readout("br_now", "Loaded")
toggle("fav", "Favorite")
button("rnd", "Random Pick")
button("copy", "Copy 1 > 2")
button("swap", "Swap 1 <> 2")

# --- the wave view (OSC tab, WAVES): each oscillator's current frame as WAVE_COLS columns, each a value the
# plugin sets and a display-only filmstrip shows (a bar from the zero line; skin_polish.py draws the strip) ---
for o in (1, 2):
    for c in range(1, WAVE_COLS + 1):
        meter_param("o%d_wv%02d" % (o, c), "O%d Wave %02d" % (o, c))


def norm(p):
    """The default as MPC's 0..1 value."""
    lo, hi, d = p["lo"], p["hi"], p["default"]
    if p["curve"] == "log":
        return math.log(d / lo) / math.log(hi / lo)
    if p["curve"] == "pow":
        return (d / hi) ** (1.0 / 3.0) if hi > 0 else 0.0
    return (d - lo) / (hi - lo) if hi > lo else 0.0


def params_json():
    out = []
    for p in P:
        k = p["kind"]
        e = {"key": p["key"], "name": p["name"]}
        if k == "readout":
            e.update(min=0, max=0, display="string", type="readout")
        elif k == "stepper":
            e.update(min=0, max=1, display="string", type="stepper")
        elif k == "button":
            e.update(min=0, max=1, momentary=True, type="trigger")
        elif k == "popup":
            e.update(options=p["options"], default=p["options"][0], popup_of=p["popup_of"], type="enum")
        elif k == "tile":
            e.update(options=p["options"], default=p["options"][0], display="string")
        elif "options" in p:
            e.update(options=p["options"], default=p["options"][p["default"]])
        else:
            e.update(min=0, max=1, default=round(norm(p), 6), display="string")
        out.append(e)
    return {"name": VST["name"], "params": out}


# --- touchscreen pages -------------------------------------------------------------------------
# The look: style=td3 cards (rounded, filled) on one flat colour; bg and box are the same, so the opaque
# image of a control never shows a box behind it. Plugin area 1280x628 at y = 86..714. Every page: a header
# row (status line from x=24; a page's own choices, if any, right-aligned to x=1256), then cards at y=158 and
# y=440 (h=270) or one full-height card (h=552), x=24 w=1232 or halves at x=24 / 648 (w=608). Nothing
# may sit in a card's title band (y .. y+44: td3 draws the title rule at y+38). skin_polish.py (run by
# `make skin` after the generator) redraws the knob strips, the trigger buttons and the stepper arrows.
PALETTE = {
    "bg": "15181d", "box": "15181d", "line": "2b323c", "ink": "e8ecf1", "ink_dim": "8e98a6",
    "ink_faint": "262c35", "accent": "3fd0c0", "accent_hi": "8cf2e6", "lcd": "0b0d11",
    "seg_inactive": "1d232b", "seg_active": "3fd0c0", "seg_active_tx": "06201c", "tile_on": "10322e",
    "btn_bg": "252d37", "title": "9aa5b3", "knob_face": "232a33", "knob_ring": "323b46", "knob_dot": "3fd0c0",
}
FONT_LABEL = "fonts/TitilliumWeb-SemiBold.ttf"   # font_label=: shadow_skin sizes the buttons with it
LIVE_FONT = "fonts/TitilliumWeb-SemiBold.ttf"    # the face MPC draws live text in (names, values)
TITLE_FONT = "fonts/TitilliumWeb-Bold.ttf"       # = SHADOW_TITLE_FONT in the Makefile: card titles, enum
                                                 # labels and segment, popup-option and button text
TITLE_SIZE = 17
THEME = ("style=td3\nfont_label=%s\ntitle_size=%d\n" % (FONT_LABEL, TITLE_SIZE)
         + "".join("theme_%s=%s\n" % kv for kv in PALETTE.items()))
TEXT_INK = PALETTE["ink_dim"]   # free bitmap text (column headers, slot numbers, hints)

S8 = [100, 252, 404, 556, 708, 860, 1012, 1164]   # 8 knob slots across a card = one Q-Link bank
L4, R4 = S8[:4], [724, 876, 1028, 1180]           # 4 slots in the left / right half card
R1, R2 = 158, 440                                 # card rows (h=270), or R1 with h=552
# The wave view: WAVE_COLS meters WAVE_PITCH apart, WAVE_H tall. shadow_skin gives each a square
# component of the larger side, centred on it, so the first and last stick out WAVE_H / 2 sideways.
# 128 at most: the strip is 128 frames of that square, and MPC draws an image taller than 16384 px wrongly.
WAVE_X0, WAVE_PITCH, WAVE_H = 120, 13, 128

# Knobs: shadow_skin bakes ONE filmstrip per radius, so the radius picks the look. A bipolar knob (its arc
# grows from 12 o'clock) is one pixel smaller than a unipolar knob of the same size. This table is the only
# place that says so: check_layout() holds every knob to it and skin_polish.py draws the strips from it
# (exported as build/skin_style.json).
KNOB_SIZES = {"big": 30, "small": 22}
KNOB_STYLES = {r - b: {"bipolar": bool(b), "track": 4 if r - b >= 28 else 3, "pointer": 3.0 if r - b >= 28 else 2.5}
               for r in KNOB_SIZES.values() for b in (0, 1)}
BIPOLAR_EXTRA = ("o1_sub_tune", "o2_sub_tune")    # -36..+12 st, centred on its default an octave down
PRIMARY_BUTTONS = ("pre_save", "xy_auto")         # drawn in the accent colour by skin_polish.py
FRAMES = 128                                      # shadow_skin: every filmstrip has 128 frames
PARAMS = {p["key"]: p for p in P}


def bipolar(key):
    """A knob whose value runs both ways from the middle (pan, fine, amounts): symmetric range."""
    p = PARAMS[key]
    return p["lo"] == -p["hi"] or key in BIPOLAR_EXTRA


def knob_radius(key, size="big"):
    return KNOB_SIZES[size] - (1 if bipolar(key) else 0)


class Layout:
    """layout.conf lines, one generator [tab] per page. Pages are grouped: a group is one button of MPC's tab strip
    and its pages are that button's sub-pages (the dots under it; a tap on the button again shows the next), each
    with its own screen and its own Q-Link set. skin_polish.py renumbers the generator's tabs into these groups
    (skin_style.json "tab_groups"). mode() tags every widget that follows with when= until the next page or mode()."""

    def __init__(self):
        self.lines, self.when, self.groups = [THEME], None, []

    def group(self, name):
        self.groups.append({"name": name, "pages": []})

    def page(self, name, qlinks):
        """A page of the current group. Its name is also its Q-Link set's title, which MPC shows in the tab strip
        while the page is up."""
        self.lines.append("[tab %s]" % name)
        self.lines.append('qlinks "%s" = %s' % (name, ",".join(qlinks)))
        self.groups[-1]["pages"].append(name)
        self.when = None

    def mode(self, when):
        self.when = when

    def add(self, line):
        self.lines.append(line + (' when="%s"' % self.when if self.when else ""))

    def header(self, modes=None, status_w=None):
        """The status line from x=24 and the page-mode selector right-aligned to x=1256."""
        n = len(PARAMS[modes]["options"]) if modes else 0
        w = status_w or (1232 - n * 124 - 16 if n else 1232)
        self.readout(24 + w // 2, 121, w, "status")
        if n:
            self.hseg(1256 - (n * 124 - 2) // 2, 121, modes, 122)

    def card(self, x, y, w, h, title):
        self.add('frame x=%d y=%d w=%d h=%d title="%s"' % (x, y, w, h, title))

    def knob(self, cx, cy, key, size="big"):
        self.add('knob cx=%d cy=%d r=%d label="%s" key=%s' % (cx, cy, knob_radius(key, size), PARAMS[key]["name"], key))

    def hseg(self, cx, cy, key, sw, label=None):
        self.add('enum_h cx=%d cy=%d sw=%d key=%s%s' % (cx, cy, sw, key, ' label="%s"' % label if label else ""))

    def vseg(self, cx, cy, key, sw=124, label=None):
        self.add('enum_v cx=%d cy=%d sw=%d key=%s%s' % (cx, cy, sw, key, ' label="%s"' % label if label else ""))

    def popup(self, cx, cy, w, key):
        self.add('popup cx=%d cy=%d w=%d h=40 key=%s' % (cx, cy, w, key))

    def stepper(self, cx, cy, w, key):
        self.add('stepper cx=%d cy=%d w=%d h=40 key=%s' % (cx, cy, w, key))

    def readout(self, cx, cy, w, key, h=40):
        self.add('readout cx=%d cy=%d w=%d h=%d key=%s' % (cx, cy, w, h, key))

    def button(self, cx, cy, label, key):
        self.add('button cx=%d cy=%d label="%s" key=%s' % (cx, cy, label, key))

    def text(self, cx, top, label):   # bitmap font; top = the top of the glyphs (render_conf_preview.c)
        self.add('text cx=%d cy=%d label="%s" color=%s' % (cx, top, label, TEXT_INK))

    def slider(self, cx, cy, w, h, cw, key):
        self.add('slider_v cx=%d cy=%d w=%d h=%d cw=%d label="%s" key=%s' % (cx, cy, w, h, cw, PARAMS[key]["name"], key))

    def toggle(self, cx, cy, key):
        self.add('toggle cx=%d cy=%d label="%s" key=%s' % (cx, cy, PARAMS[key]["name"], key))

    def meter(self, cx, cy, w, h, key):   # display only: shadow_skin's filmstrip meter, no look (PolyForce patch)
        self.add('meter cx=%d cy=%d w=%d h=%d key=%s' % (cx, cy, w, h, key))

    def tiles(self, x, y, w, cols, rows, th, gap, key):
        self.add('list x=%d y=%d w=%d cols=%d rows=%d th=%d gap=%d key=%s' % (x, y, w, cols, rows, th, gap, key))


def build_layout():
    """Every page, in groups (see Layout). Each page has its own Q-Link set: the Force's 8 knobs show the first 8
    keys, the next bank the other 8 (shadow_skin qlink_for_slot)."""
    L = Layout()
    osc8 = ("pos", "oct", "semi", "fine", "uni", "detune", "width", "level")

    # OSC: one page per oscillator, then the noise and the levels, then the wave view.
    L.group("OSC")
    for o in (1, 2):
        p = "o%d_" % o
        L.page("OSC %d" % o, [p + k for k in osc8] + [p + k for k in ("table", "pan", "phase", "phmode", "route",
                                                                      "sub_wave", "sub_tune", "sub_level")])
        L.header()
        L.card(24, R1, 1232, 270, "OSCILLATOR %d" % o)
        L.popup(144, R1 + 76, 200, p + "wave")
        L.stepper(750, R1 + 76, 972, p + "table")
        for cx, k in zip(S8, osc8):
            L.knob(cx, R1 + 164, p + k)
        L.card(24, R2, 608, 270, "OUTPUT")
        L.knob(L4[0], R2 + 126, p + "pan")
        L.knob(L4[1], R2 + 126, p + "phase")
        L.vseg(404, R2 + 160, p + "phmode", label="PHASE")
        L.vseg(556, R2 + 160, p + "route", label="ROUTE")
        L.card(648, R2, 608, 270, "SUB OSCILLATOR")
        L.vseg(R4[0], R2 + 160, p + "sub_wave", label="WAVE")
        L.knob(R4[1] + 20, R2 + 126, p + "sub_tune")
        L.knob(R4[2] + 40, R2 + 126, p + "sub_level")
    L.page("NOISE+MIX", ["noise_level", "noise_color", "noise_route", "o1_level", "o2_level", "o1_sub_level",
                         "o2_sub_level", "volume", "o1_sub_wave", "o1_sub_tune", "o2_sub_wave", "o2_sub_tune",
                         "o1_pan", "o2_pan", "o1_route", "o2_route"])
    L.header()
    L.card(24, R1, 608, 270, "NOISE")
    L.knob(L4[0], R1 + 126, "noise_level")
    L.knob(L4[1], R1 + 126, "noise_color")
    L.vseg(480, R1 + 160, "noise_route", label="ROUTE")
    L.card(648, R1, 608, 270, "MIX")
    for cx, k in zip(R4, ("o1_level", "o2_level", "o1_sub_level", "o2_sub_level")):
        L.knob(cx, R1 + 126, k)
    for o, x in ((1, 24), (2, 648)):
        p = "o%d_" % o
        L.card(x, R2, 608, 270, "SUB %d" % o)
        L.vseg(x + 100, R2 + 160, p + "sub_wave", label="WAVE")
        L.knob(x + 290, R2 + 126, p + "sub_tune")
        L.knob(x + 450, R2 + 126, p + "sub_level")
    # WAVES: both oscillators' current frames (table at the position knob) as bars; table and position beside.
    L.page("WAVES", ["o%d_%s" % (o, k) for o in (1, 2)
                     for k in ("table", "pos", "level", "wave", "uni", "detune", "width", "semi")])
    L.header()
    for o, top in ((1, R1), (2, R2)):
        p = "o%d_" % o
        L.card(24, top, 1232, 270, "OSCILLATOR %d" % o)
        for c in range(WAVE_COLS):
            L.meter(WAVE_X0 + c * WAVE_PITCH, top + 157, WAVE_PITCH, WAVE_H, "%swv%02d" % (p, c + 1))
        L.stepper(1036, top + 76, 400, p + "table")
        L.knob(936, top + 164, p + "pos")
        L.knob(1136, top + 164, p + "level")

    # FILTER: type and 5 knobs per filter; routing and engine next to the status line.
    flt5 = ("cut", "res", "env", "key", "drive")
    L.group("FILTER")
    L.page("FILTER", ["f1_" + k for k in flt5 + ("type",)] + ["routing", "engine"]
           + ["f2_" + k for k in flt5 + ("type",)] + ["e2_a", "e2_d"])
    L.header(status_w=664)
    L.hseg(817, 121, "routing", 112)
    L.hseg(1101, 121, "engine", 102)
    for f, top in ((1, R1), (2, R2)):
        L.card(24, top, 1232, 270, "FILTER %d" % f)
        L.popup(164, top + 126, 220, "f%d_type" % f)
        for cx, k in zip(S8[2:], flt5):
            L.knob(cx, top + 126, "f%d_%s" % (f, k))

    # MOD: the envelopes, both LFOs, the XY pads.
    L.group("MOD")
    adsr = ("a", "d", "s", "r", "vel")
    L.page("ENVELOPES", ["e1_" + k for k in adsr] + ["e2_loop", "f1_env", "f2_env"]
           + ["e2_" + k for k in adsr] + ["e2_pos", "f1_cut", "f2_cut"])
    L.header()
    for e, top, title in ((1, R1, "AMP ENVELOPE"), (2, R2, "MOD ENVELOPE")):
        L.card(24, top, 1232, 270, title)
        for cx, k in zip(S8, adsr):
            L.knob(cx, top + 126, "e%d_%s" % (e, k))
    L.knob(S8[5], R2 + 126, "e2_pos")
    L.vseg(1088, R2 + 160, "e2_loop", sw=140, label="LOOP")
    lfo5 = ("rate", "phase", "delay", "fade", "depth")
    L.page("LFOS", ["l%d_%s" % (l, k) for l in (1, 2) for k in lfo5 + ("wave", "sync", "div")])
    L.header()
    for l, top in ((1, R1), (2, R2)):
        p = "l%d_" % l
        L.card(24, top, 1232, 270, "LFO %d" % l)
        L.popup(144, top + 76, 200, p + "wave")
        L.hseg(380, top + 76, p + "sync", 100)
        L.popup(574, top + 76, 160, p + "div")
        L.hseg(800, top + 76, p + "polar", 120)
        L.hseg(1090, top + 76, p + "trig", 104)
        for cx, k in zip(S8, lfo5):
            L.knob(cx, top + 164, p + k)
    xy = ["xy%d_%s" % (x, a) for x in range(1, 5) for a in "xy"]
    L.page("XY PADS", xy)
    L.header()
    L.card(24, R1, 1232, 270, "XY PADS")
    for cx, k in zip(S8, xy):
        L.knob(cx, R1 + 126, k)
    L.card(24, R2, 1232, 270, "ASSIGN")
    L.button(164, R2 + 126, "AUTO-ASSIGN", "xy_auto")
    L.text(700, R2 + 116, "FREE PADS GO TO FREE MATRIX SLOTS")

    # MATRIX: 12 slots, four per page (source, via, two targets with amounts); the 12 modifiers on their own.
    # A page per four slots also keeps each page light: every popup option is a component MPC builds when the
    # page opens (all twelve slots on one page took ~0.9 s on the Force).
    L.group("MATRIX")
    rows = [268 + 110 * r for r in range(4)]   # 110: a small knob's box is 110 tall (the mock had 108)
    for page, name in enumerate(["1-4", "5-8", "9-12"]):
        slots4 = range(page * 4 + 1, page * 4 + 5)
        L.page("MATRIX " + name, ["m%d_%s" % (k, s) for s in ("a1", "a2", "src", "t1") for k in slots4])
        L.header()
        L.card(24, R1, 1232, 552, "MOD SLOTS %s" % name)
        for label, cx in (("SOURCE", 170), ("VIA", 380), ("TARGET 1", 600), ("AMOUNT", 772), ("TARGET 2", 950),
                          ("AMOUNT", 1124)):
            L.text(cx, 202, label)
        for r, cy in enumerate(rows):
            k = page * 4 + r + 1
            p = "m%d_" % k
            L.text(56, cy - 8, str(k))
            L.popup(170, cy, 190, p + "src")
            L.popup(380, cy, 190, p + "via")
            L.popup(600, cy, 200, p + "t1")
            L.knob(772, cy, p + "a1", "small")
            L.popup(950, cy, 200, p + "t2")
            L.knob(1124, cy, p + "a2", "small")
    L.page("MODIFIERS", ["m%d_modamt" % k for k in range(1, MOD_SLOTS + 1)])
    L.header()
    L.card(24, R1, 1232, 552, "MODIFIERS")
    for k in range(1, MOD_SLOTS + 1):
        x, cy = 24 + (k - 1) // 4 * 410, rows[(k - 1) % 4]
        L.text(x + 34, cy - 8, str(k))
        L.popup(x + 150, cy, 170, "m%d_mod" % k)
        L.knob(x + 320, cy, "m%d_modamt" % k, "small")

    # BROWSE: categories left, tables or presets right, the loaded item and actions below.
    L.group("BROWSE")
    L.page("BROWSE", ["o1_table", "o1_pos", "o1_level", "o1_detune", "o2_table", "o2_pos", "o2_level", "o2_detune",
                      "preset", "tuning", "f1_cut", "f1_res", "f2_cut", "f2_res", "rand_amt", "volume"])
    L.header("br_target")
    L.card(24, R1, 360, 552, "CATEGORIES")
    L.tiles(44, 206, 320, 2, 8, 48, 8, "cat")
    L.button(124, 676, "< PREV", "cat_prev")
    L.button(304, 676, "NEXT >", "cat_next")
    for target, title in (("OSC 1", "TABLES"), ("OSC 2", "TABLES"), ("PRESETS", "PRESETS")):
        L.mode("br_target:%s" % target)   # only the card title changes; the page readout is drawn over the card
        L.card(400, R1, 856, 474, title)
        L.readout(828, 596, 240, "tbl_page", h=36)
    L.mode(None)
    L.tiles(420, 206, 816, 3, 8, 38, 8, "tbl")
    L.button(476, 596, "< PREV", "tbl_prev")
    L.button(1180, 596, "NEXT >", "tbl_next")
    L.readout(605, 676, 410, "br_now")   # 410, not the mock's 500: the toggle and 3 buttons need 430 px
    L.toggle(874, 668, "fav")
    L.button(989, 676, "RND", "rnd")
    L.button(1094, 676, "1>2", "copy")
    L.button(1201, 676, "1<>2", "swap")

    # VOICE: how notes become voices; glide; presets and tuning.
    L.group("VOICE")
    voice6 = ("voices", "volume", "vel_curve", "e1_vel", "bend_up", "bend_dn")
    L.page("VOICE", list(voice6) + ["vmode", "steal", "same_note", "glide_mode", "glide_type", "glide",
                                    "preset", "tuning", "rand_amt"])
    L.header()
    L.card(24, R1, 1232, 270, "VOICES")
    L.hseg(264, R1 + 96, "vmode", 110, label="MODE")
    L.hseg(740, R1 + 96, "steal", 116, label="STEAL")
    L.hseg(1120, R1 + 96, "same_note", 120, label="SAME NOTE")
    for cx, k in zip(S8, voice6):
        L.knob(cx, R1 + 172, k)
    L.card(24, R2, 608, 270, "GLIDE")
    L.vseg(124, R2 + 160, "glide_mode", label="GLIDE")
    L.vseg(290, R2 + 160, "glide_type", label="TYPE")
    L.knob(480, R2 + 126, "glide")
    L.card(648, R2, 608, 270, "PATCH")
    L.stepper(888, R2 + 76, 440, "preset")
    L.stepper(888, R2 + 128, 440, "tuning")
    L.button(730, R2 + 204, "SAVE", "pre_save")
    L.button(846, R2 + 204, "INIT", "pre_init")
    L.button(980, R2 + 204, "RANDOM", "pre_rand")
    L.knob(1186, R2 + 76, "rand_amt", "small")

    # SEQ: arpeggiator / sequencer settings, the 16 steps, the shape lanes two at a time.
    L.group("SEQ")
    arp4 = ("arp_oct", "clk_gate", "clk_swing", "seq_steps")
    L.page("ARP/SEQ", list(arp4) + ["seq_mode", "arp_dir", "clk_rate", "arp_latch", "arp_pattern", "sh_rate",
                                    "sh_steps"] + ["sh%d_mode" % l for l in range(1, 5)])
    L.header()
    L.card(24, R1, 1232, 270, "ARPEGGIATOR / SEQUENCER")
    L.hseg(190, R1 + 96, "seq_mode", 96, label="MODE")
    L.hseg(470, R1 + 96, "arp_latch", 100, label="LATCH")
    L.hseg(730, R1 + 96, "arp_pattern", 100, label="PATTERN")
    L.hseg(990, R1 + 96, "seq_rec", 100, label="RECORD")
    for cx, k in zip(S8, arp4):
        L.knob(cx, R1 + 172, k)
    L.popup(870, R1 + 172, 200, "arp_dir")
    L.popup(1110, R1 + 172, 200, "clk_rate")
    L.card(24, R2, 1232, 270, "SHAPE SEQUENCER")
    L.popup(164, R2 + 126, 200, "sh_rate")
    L.knob(404, R2 + 126, "sh_steps")
    L.text(830, R2 + 116, "FOUR LANES OF EIGHT STEPS  SOURCES SHAPE 1-4")
    # STEPS: all three rows on one screen; the Q-Links play the notes (velocity and mod by touch).
    L.page("STEPS", ["s%d_note" % k for k in range(1, SEQ_STEPS + 1)])
    L.header()
    for key, title, top in (("note", "NOTE", 158), ("vel", "VELOCITY", 344), ("mod", "MOD", 530)):
        L.card(24, top, 1232, 180, title)
        for k in range(1, SEQ_STEPS + 1):   # h=76 (mock 84): the slider's 56 px of labels fit the 180 px card
            L.slider(81 + (k - 1) * 74, top + 86, 18, 76, 72, "s%d_%s" % (k, key))
    for a in (1, 3):
        L.page("SHAPES %d-%d" % (a, a + 1), ["sh%d_%d" % (l, k) for l in (a, a + 1) for k in range(1, SHAPE_STEPS + 1)])
        L.header()
        for l, top in ((a, R1), (a + 1, R2)):
            L.card(24, top, 1232, 270, "SHAPE %d" % l)
            L.vseg(110, top + 150, "sh%d_mode" % l, sw=120, label="MODE")
            for k in range(1, SHAPE_STEPS + 1):
                L.slider(300 + (k - 1) * 130, top + 126, 24, 120, 110, "sh%d_%d" % (l, k))
    return L


def pages():
    """layout.conf."""
    return "\n".join(build_layout().lines) + "\n"


def skin_style():
    """What skin_polish.py needs (build/skin_style.json): the palette, knob looks and primary buttons to redraw the
    knob strips, buttons and stepper arrows, and the page groups to renumber the generator's tabs into sub-pages."""
    return {"palette": PALETTE, "title_font": TITLE_FONT, "frames": FRAMES,
            "knobs": {str(r): s for r, s in sorted(KNOB_STYLES.items())}, "primary_buttons": list(PRIMARY_BUTTONS),
            "tab_groups": build_layout().groups}


# --- layout check (offline, no skin toolchain) ---------------------------------------------------
# Geometry mirrors third_party/mpc-vst-plugins/tools/shadow_skin.py (component boxes, button_rect,
# seg_rects, popup_layout) and render_conf_preview.c (the bitmap font of `text`).
X0, Y0, X1, Y1 = 0, 86, 1280, 714
TITLE_BAND = 44              # a card's title band: y .. y+44
NAME_MAX = 13                # MPC shows a knob/slider's effGetParamName at ~19.5 px in a 130 px box
MAX_IMAGE_H = 16384          # taller skin images draw wrongly (sd88me/mpc-vst-plugins catalog_check.py warns)
POP_ROW, POP_GAP, POP_PAD, POP_GROUP_ROWS = 40, 2, 6, 8
BITMAP_GLYPHS = " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-/_>%+:#"   # font8x8.h font_chars
BITMAP_ADVANCE = {" ": 4, "J": 9, "j": 9, ".": 9, "-": 9, ":": 9}   # font_glyph_width(): last lit column + 2; else 10
INT_KEYS = ("x", "y", "w", "h", "cx", "cy", "r", "sw", "rows", "cols", "th", "gap", "cw")


def _widget(line):
    toks = shlex.split(line)
    w = {"kind": toks[0]}
    for t in toks[1:]:
        k, _, v = t.partition("=")
        w[k] = int(v) if k in INT_KEYS else v
    return w


def bitmap_width(s, scale):
    """render_conf_preview.c text_width()."""
    return int(sum(BITMAP_ADVANCE.get(c, 10) * scale for c in s))


_FONTS = {}


def _ttf_advances(path):
    """(unitsPerEm, {char: advance}) from a TrueType font's cmap (format 4) and hmtx tables."""
    import struct
    d = open(path, "rb").read()
    tabs = {}
    for i in range(struct.unpack(">H", d[4:6])[0]):
        tag, _, off, ln = struct.unpack(">4sIII", d[12 + 16 * i:28 + 16 * i])
        tabs[tag.decode("latin-1")] = off
    upem = struct.unpack(">H", d[tabs["head"] + 18:tabs["head"] + 20])[0]
    nhm = struct.unpack(">H", d[tabs["hhea"] + 34:tabs["hhea"] + 36])[0]
    adv = [struct.unpack(">H", d[tabs["hmtx"] + 4 * i:tabs["hmtx"] + 4 * i + 2])[0] for i in range(nhm)]
    co = tabs["cmap"]
    for i in range(struct.unpack(">H", d[co + 2:co + 4])[0]):
        pid, eid, off = struct.unpack(">HHI", d[co + 4 + 8 * i:co + 12 + 8 * i])
        so = co + off
        if struct.unpack(">H", d[so:so + 2])[0] != 4 or (pid, eid) not in ((3, 1), (0, 3), (0, 4)):
            continue
        n2 = struct.unpack(">H", d[so + 6:so + 8])[0]
        ends = struct.unpack(">%dH" % (n2 // 2), d[so + 14:so + 14 + n2])
        starts = struct.unpack(">%dH" % (n2 // 2), d[so + 16 + n2:so + 16 + 2 * n2])
        deltas = struct.unpack(">%dh" % (n2 // 2), d[so + 16 + 2 * n2:so + 16 + 3 * n2])
        ro = so + 16 + 3 * n2
        ranges = struct.unpack(">%dH" % (n2 // 2), d[ro:ro + n2])
        out = {}
        for s in range(n2 // 2):
            for c in range(starts[s], min(ends[s], 0x7e) + 1):
                if ranges[s]:
                    gi = ro + 2 * s + ranges[s] + 2 * (c - starts[s])
                    g = struct.unpack(">H", d[gi:gi + 2])[0]
                    g = (g + deltas[s]) & 0xFFFF if g else 0
                else:
                    g = (c + deltas[s]) & 0xFFFF
                out[chr(c)] = adv[min(g, nhm - 1)]
        return upem, out
    raise SystemExit("%s: no Unicode cmap" % path)


def ttf_width(font, px, s):
    """Advance width of s in a bundled font at px pixels, measured with Pillow as shadow_skin does. Without
    Pillow (surface.py needs only python3): from the font's own advance table, +1 px (that is within 0.75 px
    of Pillow for Titillium Web, and errs wide)."""
    key = (font, px)
    if key not in _FONTS:
        path = os.path.join(HERE, font)
        try:
            from PIL import ImageFont
            _FONTS[key] = ImageFont.truetype(path, px).getlength
        except ImportError:
            upem, adv = _ttf_advances(path)
            _FONTS[key] = lambda t: sum(adv.get(c, upem) for c in t) * px / upem + 1.0
    return _FONTS[key](s)


def _top_level(text):
    """The style keys before the first [tab] (shadow_skin apply_theme)."""
    top = {}
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("["):
            break
        k, eq, v = line.partition("=")
        if eq and not line.startswith("#"):
            top[k.strip()] = v.strip()
    return top


class Geometry:
    """Where shadow_skin puts each widget, for this layout's style keys."""

    def __init__(self, top):
        self.td3 = top.get("style") == "td3"
        self.font_label = top.get("font_label")
        self.ls = float(top.get("label_scale", 1.15))

    def knob(self, w):   # shadow_skin build(): the filmstrip, the Name and Value labels under it
        r = w["r"]
        s = 2 * r + 10
        cw = max(130, s)
        name_y = s // 2 + r + 2
        ch = name_y + round(20 * self.ls) + 2 + round(26 * self.ls) + 6
        return (w["cx"] - cw // 2, w["cy"] - s // 2, cw, ch)

    def slider(self, w):
        sq = max(w["w"], w["h"])
        cw = w.get("cw", max(130, sq))
        ch = (sq - w["h"]) // 2 + w["h"] + 2 + 20 + 2 + 26 + 6
        return (w["cx"] - cw // 2, w["cy"] - sq // 2, cw, ch)

    def text_width(self, s):   # shadow_skin text_width(): sizes a button
        if self.font_label:
            return int(ttf_width(self.font_label, round(9 * 1.15 * 1.6), s) * 1.2)
        return int(len(s) * 10 * 1.15 - 1.15)

    def button(self, w):   # shadow_skin button_rect()
        bw, bh = self.text_width(w["label"]) + 36, 39
        if self.td3:
            bw, bh = bw + 24 + 4, 48 + 4
        return (w["cx"] - bw // 2, w["cy"] - bh // 2, bw, bh)

    @staticmethod
    def segs(w, n):   # shadow_skin seg_rects()
        if w["kind"] == "enum_v":
            sw = w.get("sw") or 135
            y0 = w["cy"] - (n * 32) // 2
            return [(w["cx"] - sw // 2, y0 + i * 32, sw, 30) for i in range(n)]
        sw, rows = w.get("sw") or 117, w.get("rows", 1)
        per = -(-n // rows)
        out = []
        for i in range(n):
            r, c = divmod(i, per)
            cnt = min(per, n - r * per)
            out.append((w["cx"] - (cnt * sw + (cnt - 1) * 2) // 2 + c * (sw + 2), w["cy"] - 16 + r * 35, sw, 33))
        return out

    @staticmethod
    def enum_label(w, n):   # the TrueType group label shadow_skin draws centred at (gx, gy), 18 px
        gy = w["cy"] - 33 // 2 - 22 if w["kind"] == "enum_h" else w["cy"] - (n * 32) // 2 - 24
        tw = int(ttf_width(TITLE_FONT, 18, w["label"])) + 2
        return (w["cx"] - tw // 2, gy - 10, tw, 20)

    @staticmethod
    def text(w):   # render_conf_preview.c draw_text_c(): cx centres, cy is the TOP of the glyphs
        size = float(w.get("size", 1.5))
        tw = bitmap_width(w["label"], size)
        return (w["cx"] - tw // 2, w["cy"], tw + 1, int(9 * size + 0.5))

    @staticmethod
    def popup_panel(w, n):   # shadow_skin popup_layout(): the open list
        fx, fy, fw, fh = w["cx"] - w["w"] // 2, w["cy"] - w["h"] // 2, w["w"], w["h"]
        below, above = Y1 - (fy + fh + 4), fy - 4 - Y0
        groups = [(t, int(c)) for t, _, c in (g.rpartition(":") for g in w["groups"].split(","))] \
            if w.get("groups") else None
        if groups:
            rows = min(POP_GROUP_ROWS, max(c for _, c in groups))
            cols = sum(-(-c // rows) for _, c in groups)
            ph = (rows + 1) * (POP_ROW + POP_GAP) - POP_GAP + 2 * POP_PAD
        else:
            for cols in ([int(w["cols"])] if w.get("cols") else range(1, n + 1)):
                rows = -(-n // cols)
                ph = rows * (POP_ROW + POP_GAP) - POP_GAP + 2 * POP_PAD
                if ph <= max(below, above):
                    break
        pw = cols * fw + (cols - 1) * POP_GAP + 2 * POP_PAD
        py = fy + fh + 4 if ph <= below else fy - 4 - ph if ph <= above else Y0
        return (max(0, min(fx, X1 - pw)), py, pw, ph)

    def rects(self, w, params):
        """[(x, y, w, h)] of a control, as shadow_skin places it (stepper: arrows and text together)."""
        k = w["kind"]
        if k == "knob":
            return [self.knob(w)]
        if k in ("slider_v", "slider_h"):
            return [self.slider(w)]
        if k == "toggle":
            return [(w["cx"] - 60, w["cy"] - 18, 120, 58)]
        if k == "button":
            return [self.button(w)]
        if k in ("readout", "stepper", "popup", "menu"):
            return [(w["cx"] - w["w"] // 2, w["cy"] - w["h"] // 2, w["w"], w["h"])]
        if k == "list":
            tw = (w["w"] - (w["cols"] - 1) * w["gap"]) // w["cols"]
            return [(w["x"] + c * (tw + w["gap"]), w["y"] + r * (w["th"] + w["gap"]), tw, w["th"])
                    for r in range(w["rows"]) for c in range(w["cols"])]
        if k in ("enum_h", "enum_v"):
            return self.segs(w, len(params[w["key"]]["options"]))
        if k == "meter":   # shadow_skin: a square of the larger side, centred (transparent padding)
            sq = max(w["w"], w["h"])
            return [(w["cx"] - sq // 2, w["cy"] - sq // 2, sq, sq)]
        return []


def _overlap(a, b):
    return a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and a[1] < b[1] + b[3] and b[1] < a[1] + a[3]


def _inside(r):
    return r[0] >= X0 and r[1] >= Y0 and r[0] + r[2] <= X1 and r[1] + r[3] <= Y1


def _same_screen(m1, m2):
    return m1 is None or m2 is None or m1 == m2


def check_names(layout_tabs, geo, errors):
    """Parameter names: MPC shows them under knobs and sliders and in its Q-Link overlay, without page context."""
    seen = {}
    for p in P:
        if len(p["name"]) > 24:
            errors.append("parameter %s: name %r is longer than 24 characters" % (p["key"], p["name"]))
        if p["name"].lower() in seen:
            errors.append("parameters %s and %s have the same name %r" % (seen[p["name"].lower()], p["key"], p["name"]))
        seen[p["name"].lower()] = p["key"]
    for tab in layout_tabs:
        for w in tab["widgets"]:
            if w["kind"] not in ("knob", "slider_v", "slider_h", "toggle") or w.get("key") not in PARAMS:
                continue
            name = PARAMS[w["key"]]["name"]
            if len(name) > NAME_MAX:
                errors.append("%s: %s %s: name %r is longer than %d characters" % (tab["name"], w["kind"], w["key"], name,
                                                                                    NAME_MAX))
            # the live Name label: knob 17 x label_scale px in max(130, 2r+10); slider 17 px in cw; toggle 15 px in 120
            px, box = ((math.ceil(17 * geo.ls), max(130, 2 * w["r"] + 10)) if w["kind"] == "knob" else
                       (15, 120) if w["kind"] == "toggle" else (17, w.get("cw") or max(130, w["w"], w["h"])))
            if ttf_width(LIVE_FONT, px, name) > box - 4:
                errors.append("%s: %s %s: name %r does not fit its %d px label" % (tab["name"], w["kind"], w["key"], name, box))


def check_layout(text, groups):
    """Raise SystemExit on anything shadow_skin.py would refuse, plus geometry mistakes: outside the plugin
    area, overlaps on one screen (a page mode with everything shown in every mode), controls or text in a
    card's title band, open popup lists that leave the plugin area, unknown bitmap glyphs; and the page groups
    (Layout): every page in one group, in layout order, with exactly one Q-Link set titled like the page."""
    params = PARAMS
    geo = Geometry(_top_level(text))
    errors = []
    tabs = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or (not tabs and "=" in line and not line.startswith("[")):
            continue
        m = re.match(r"\[tab (.+)\]$", line)
        if m:
            tabs.append({"name": m.group(1), "widgets": [], "qlinks": []})
            continue
        if line.startswith("qlinks"):
            m = re.match(r'qlinks\s+"([^"]+)"\s*=\s*(.+)$', line)
            keys = [k.strip() for k in m.group(2).split(",") if k.strip()]
            tabs[-1]["qlinks"].append((m.group(1), keys))
            continue
        tabs[-1]["widgets"].append(_widget(line))
    if len(groups) > 7:
        errors.append("%d page groups: MPC's tab strip shows five plus a pager; keep it to seven" % len(groups))
    errors += ["page group %s has no pages" % g["name"] for g in groups if not g["pages"]]
    grouped, names = [n for g in groups for n in g["pages"]], [t["name"] for t in tabs]
    if grouped != names:
        errors.append("the page groups list %s, the layout has the pages %s" % (grouped, names))
    errors += ["page %r: the name is taken (MPC tells pages apart by it)" % n for n in sorted(set(names))
               if names.count(n) > 1]
    for t in tabs:
        if [title for title, _ in t["qlinks"]] != [t["name"]]:
            errors.append("%s: a page has exactly one Q-Link set, titled like the page" % t["name"])
    check_names(tabs, geo, errors)
    errors += ["PRIMARY_BUTTONS: %r is not a button parameter" % k for k in PRIMARY_BUTTONS
               if PARAMS.get(k, {}).get("kind") != "button"]
    errors += ["BIPOLAR_EXTRA: %r is not a parameter" % k for k in BIPOLAR_EXTRA if k not in PARAMS]
    seg_images = {}   # shadow_skin names enum images sh_seg_<key>_<n> for the whole skin: one size per key
    for tab in tabs:
        T = tab["name"]
        frames, placed = [], []   # (rect, title, mode); (rect, what, mode)
        for w in tab["widgets"]:
            kind, key, mode = w["kind"], w.get("key"), w.get("when")
            if mode:
                mk, _, mo = mode.partition(":")
                opts = [o.lower() for o in params.get(mk, {}).get("options", [])]
                if len(opts) < 2 or mo.lower() not in opts:
                    errors.append("%s: when=%s is not an option of an option parameter" % (T, mode))
            if kind == "frame":
                r = (w["x"], w["y"], w["w"], w["h"])
                if not _inside(r):
                    errors.append("%s: frame %r at %s leaves the plugin area" % (T, w.get("title"), r))
                frames.append((r, w.get("title", ""), mode))
                continue
            if kind == "text":
                lab = w.get("label", "")
                bad = sorted(set(c for c in lab if c not in BITMAP_GLYPHS))
                if not lab or bad:
                    errors.append("%s: text %r: %s" % (T, lab, "the bitmap font has no %r" % "".join(bad) if bad
                                                       else "an empty label fails shadow_art"))
                placed.append((Geometry.text(w), "text %r" % lab, mode))
                continue
            if kind == "art":
                errors.append("%s: art needs the browser renderer" % T)
                continue
            need = ["%s_%d" % (key, i + 1) for i in range(w["cols"] * w["rows"])] if kind == "list" else [key]
            if kind == "stepper":
                need += [key + "_prev", key + "_next"]
            if kind == "popup":
                need.append(key + "__open")
            missing = [k for k in need if k not in params]
            for k in missing:
                errors.append("%s: %s key %r is not a parameter" % (T, kind, k))
            if missing:
                continue
            p = params[need[0]]
            if kind in ("enum_h", "enum_v", "popup") and "options" not in p:
                errors.append("%s: %s %r is not an option parameter" % (T, kind, key))
                continue
            if kind == "list" and p["kind"] != "tile":
                errors.append("%s: list %r tiles must be tile parameters" % (T, key))
            if kind == "stepper" and p["kind"] != "stepper":
                errors.append("%s: stepper %r is not a stepper parameter" % (T, key))
            if kind == "meter" and p["kind"] != "meter":
                errors.append("%s: meter %r is not a meter parameter" % (T, key))
            if kind == "button" and not w.get("label"):
                errors.append("%s: button %r needs a label" % (T, key))
                continue
            strip = {"knob": lambda: 2 * w["r"] + 10, "slider_v": lambda: max(w["w"], w["h"]),
                     "slider_h": lambda: max(w["w"], w["h"]), "meter": lambda: max(w["w"], w["h"])}.get(kind)
            if strip and strip() * FRAMES > MAX_IMAGE_H:   # FRAMES square frames stacked: one tall image
                errors.append("%s: %s %s: its filmstrip is %d px tall; MPC draws images over %d px wrongly" % (
                    T, kind, key, strip() * FRAMES, MAX_IMAGE_H))
            if kind == "knob":
                style = KNOB_STYLES.get(w["r"])
                if not style:
                    errors.append("%s: knob %s: r=%d has no look in KNOB_STYLES" % (T, key, w["r"]))
                elif style["bipolar"] != bipolar(key):
                    errors.append("%s: knob %s: r=%d is a %s look, the parameter is %s" % (
                        T, key, w["r"], "bipolar" if style["bipolar"] else "unipolar",
                        "bipolar" if bipolar(key) else "unipolar"))
            if kind in ("readout", "stepper", "popup") and w["h"] < 36:
                errors.append("%s: %s %s: h=%d clips its 26 px live text (min 36)" % (T, kind, key, w["h"]))
            if kind in ("enum_h", "enum_v"):
                n = len(p["options"])
                size = (kind, w.get("sw"), n)
                if seg_images.setdefault(key, size) != size:
                    errors.append("%s: enum %s is drawn as %s and %s: its segment images are shared" % (
                        T, key, seg_images[key], size))
                if w.get("label"):
                    placed.append((Geometry.enum_label(w, n), "%s label" % key, mode))
            if kind == "popup":
                panel = Geometry.popup_panel(w, len(p["options"]))
                if not _inside(panel):
                    errors.append("%s: popup %s: its open list %s leaves the plugin area" % (T, key, panel))
            for r in geo.rects(w, params):
                placed.append((r, "%s %s" % (kind, key), mode))
        for i, (r, what, mode) in enumerate(placed):
            if not _inside(r):
                errors.append("%s: %s at %s leaves the plugin area" % (T, what, r))
            for o_r, o_what, o_mode in placed[:i]:
                if what.startswith("meter ") and o_what.startswith("meter "):
                    continue   # a row of meters: their padded squares overlap, transparent and untouchable
                if _same_screen(mode, o_mode) and _overlap(r, o_r) and o_what != what:
                    errors.append("%s: %s overlaps %s" % (T, what, o_what))
            for f_r, title, f_mode in frames:
                band = (f_r[0], f_r[1], f_r[2], TITLE_BAND)
                if _same_screen(mode, f_mode) and _overlap(r, band):
                    errors.append("%s: %s at %s is in the title band of card %r" % (T, what, r, title))
        titles = [t for t, _ in tab["qlinks"]]
        for title, keys in tab["qlinks"]:
            if len(keys) > 16:
                errors.append("%s: qlinks %r has %d keys (max 16)" % (T, title, len(keys)))
            if len(title) > 12 or titles.count(title) > 1:
                errors.append("%s: qlinks title %r: keep it unique and at most 12 characters (MPC's tab strip)" % (T, title))
            for k in keys:
                if k not in params:
                    errors.append("%s: qlinks %r key %r is not a parameter" % (T, title, k))
    if errors:
        raise SystemExit("layout check failed:\n  " + "\n  ".join(errors))
    return tabs


# --- C++ header --------------------------------------------------------------------------------
CURVE = {"readout": "Readout", "enum": "Enum", "lin": "Lin", "log": "Log", "int": "Int", "pow": "Pow"}
FMT = {"none": "None", "enum": "Enum", "pct": "Percent", "bipct": "Bipolar", "hz": "Hz", "time": "Time",
       "semi": "Semi", "cent": "Cent", "oct": "Oct", "count": "Count", "db": "Db", "detune": "Detune",
       "text": "Text", "frame1": "Frame1", "frame2": "Frame2", "pan": "Pan", "deg": "Degrees", "lfohz": "LfoHz",
       "modamt": "ModAmt"}
KIND = {"synth": "Synth", "ui": "Ui", "readout": "Readout", "stepper": "Stepper", "button": "Button",
        "tile": "Tile", "toggle": "Toggle", "popup": "Popup", "meter": "Meter"}


def c_str(s):
    return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"') + '"'


def header():
    index = {p["key"]: i for i, p in enumerate(P)}
    ids = ",\n".join("    P_%s%s" % (p["key"].upper(), " = 0" if i == 0 else "") for i, p in enumerate(P))
    specs = ",\n".join("    {Curve::%s, Fmt::%s, %sf, %sf}  /* %s */" % (
        CURVE[p["curve"]], FMT[p["fmt"]], float(p["lo"]), float(p["hi"]), p["key"]) for p in P)
    opts = []
    for i, p in enumerate(P):
        if "options" in p:
            opts.append("static constexpr const char* OPTS_%d[] = {%s};" % (i, ", ".join(c_str(o) for o in p["options"])))
    info = ",\n".join("    {%s, %s, Kind::%s, %rf, %d, %s, %d}" % (
        c_str(p["key"]), c_str(p["name"]), KIND[p["kind"]], float(round(norm(p), 6)),
        len(p.get("options", [])), "OPTS_%d" % i if "options" in p else "nullptr",
        index[p["popup_of"]] if p["kind"] == "popup" else -1) for i, p in enumerate(P))
    uid = int.from_bytes(VST["uid"].encode(), "big")
    return """// generated by surface/surface.py: do not edit
#pragma once
#include <cstdint>

namespace pf {

enum ParamId : int {
%s,
    P_COUNT
};

enum class Curve : unsigned char { Readout, Enum, Lin, Log, Int, Pow };
enum class Fmt : unsigned char { None, Enum, Percent, Bipolar, Hz, Time, Semi, Cent, Oct, Count, Db, Detune, Text,
                                 Frame1, Frame2, Pan, Degrees, LfoHz, ModAmt };
// Who owns the value and what a set does: see surface.py "kind".
enum class Kind : unsigned char { Synth, Ui, Readout, Stepper, Button, Tile, Toggle, Popup, Meter };

struct ParamSpec { Curve curve; Fmt fmt; float lo, hi; };
struct ParamInfo {
    const char* key;
    const char* name;
    Kind kind;
    float def;                  // MPC's 0..1 default
    int nopts;
    const char* const* opts;
    int popupOf;                // Kind::Popup: the parameter whose list it opens, else -1
};

static constexpr ParamSpec PARAM_SPECS[P_COUNT] = {
%s
};

%s

static constexpr ParamInfo PARAM_INFO[P_COUNT] = {
%s
};

constexpr const char* kPlugName = %s;
constexpr const char* kPlugVendor = %s;
constexpr int32_t kPlugUid = 0x%08x;   // '%s'
constexpr int32_t kPlugVersion = %d;

constexpr int kNumFilterTypes = %d;
constexpr int kParamMaxVoices = %d;   // = kMaxVoices in dsp/synth.h
constexpr int kParamMaxUnison = %d;   // = kMaxUnison in dsp/synth.h
constexpr int kStepperRange = %d;
constexpr int kBrowserCats = %d;
constexpr int kBrowserItems = %d;
constexpr int kNumLfoWaves = %d;
constexpr int kNumSyncDivisions = %d;
constexpr int kNumModSources = %d;
constexpr int kNumModTargets = %d;
constexpr int kNumModModifiers = %d;
constexpr int kNumModSlots = %d;
constexpr int kNumArpDirs = %d;
constexpr int kNumSeqSteps = %d;
constexpr int kNumShapeSteps = %d;
constexpr int kWaveCols = %d;   // the wave view's columns per oscillator: P_O1_WV01.., P_O2_WV01..

} // namespace pf
""" % (ids, specs, "\n".join(opts), info, c_str(VST["name"]), c_str(VST["vendor"]), uid, VST["uid"],
       VST["version"], len(FILTER_TYPES), MAX_VOICES, MAX_UNISON, STEPPER_RANGE, BROWSER_CATS, BROWSER_ITEMS,
       len(LFO_WAVES), len(SYNC_DIVS), len(MOD_SOURCES), len(MOD_TARGETS), len(MODIFIERS), MOD_SLOTS,
       len(ARP_DIRS), SEQ_STEPS, SHAPE_STEPS, WAVE_COLS)


# --- factory presets: presets/Factory/<NN_Category>/<NN_Name>.pfp, embedded in the .so ---------
PRESET_DIR = os.path.join(HERE, "..", "presets", "Factory")
STATE_EXTRA_KEYS = ("o1_table", "o2_table", "tuning", "preset")
PRESET_NAME_MAX = 18      # an item tile on the browser page (816 px / 3 columns)
CATEGORY_NAME_MAX = 12    # a category tile (320 px / 2 columns), shown in capitals
WAVETABLE_CPP = os.path.join(HERE, "..", "dsp", "wavetable.cpp")


def builtin_tables():
    """The built-in table names, from kRecipes in dsp/wavetable.cpp."""
    text = open(WAVETABLE_CPP, encoding="utf-8").read()
    body = text[text.index("kRecipes[] = {"):]
    body = body[:body.index("};")]
    return re.findall(r'\{"([^"]+)",\s*make\w+\}', body)


def _shown(entry):
    """"02_Supersaw_Lead.pfp" -> "Supersaw Lead", "03_Bass" -> "Bass"."""
    return re.sub(r"^\d+\s+", "", re.sub(r"\.pfp$", "", entry).replace("_", " "))


def factory_presets():
    """[(category, name, text)]: one folder per category, both in file order ("NN_" orders them, "_" shows as a
    space). Every line must be a sound parameter (or a built-in table / tuning key) with a value in range, every name
    unique (keys are "builtin:<name>") and short enough for its tile: a typo fails the build, not the device."""
    params = {p["key"]: p for p in P}
    tables = set(builtin_tables())
    out, errors, seen = [], [], {}
    for d in sorted(os.listdir(PRESET_DIR)):
        folder = os.path.join(PRESET_DIR, d)
        if d.endswith(".pfp"):
            errors.append("%s: put it in a category folder (presets/Factory/NN_Category/)" % d)
            continue
        if not os.path.isdir(folder):
            continue
        category = _shown(d)
        if not category or len(category) > CATEGORY_NAME_MAX:
            errors.append("%s: a category name of 1..%d characters" % (d, CATEGORY_NAME_MAX))
        for f in sorted(os.listdir(folder)):
            if not f.endswith(".pfp"):
                continue
            where = "%s/%s" % (d, f)
            text = open(os.path.join(folder, f), encoding="utf-8").read().replace("\r\n", "\n")
            lines = text.split("\n")
            if not lines[0].startswith("polyforce "):
                errors.append("%s: no 'polyforce N' header" % where)
            for n, line in enumerate(lines[1:], 2):
                if not line.strip():
                    continue
                key, _, val = line.partition("=")
                if key in ("o1_table", "o2_table"):
                    if not (val.startswith("builtin:") and val[8:] in tables):
                        errors.append("%s:%d: %r is not a built-in table (factory presets can't use files)" % (where, n, val))
                    continue
                if key in STATE_EXTRA_KEYS:
                    continue
                p = params.get(key)
                if not p or p["kind"] != "synth":
                    errors.append("%s:%d: %r is not a sound parameter" % (where, n, key))
                    continue
                try:
                    v = float(val)
                except ValueError:
                    errors.append("%s:%d: %r is not a number" % (where, n, val))
                    continue
                lo, hi = p["lo"], p["hi"]
                if not (min(lo, hi) - 1e-9 <= v <= max(lo, hi) + 1e-9):
                    errors.append("%s:%d: %s=%s outside %s..%s" % (where, n, key, val, lo, hi))
            name = _shown(f)
            if name in seen:
                errors.append("%s: the name %r is taken by %s" % (where, name, seen[name]))
            seen[name] = where
            if len(name) > PRESET_NAME_MAX:
                errors.append("%s: %r is longer than %d characters" % (where, name, PRESET_NAME_MAX))
            out.append((category, name, text))
    if "Init" not in seen:
        errors.append("no Init preset (the INIT button loads builtin:Init)")
    if errors:
        raise SystemExit("factory presets:\n  " + "\n  ".join(errors))
    return out


def presets_header(presets):
    rows = ",\n".join("    {%s, %s, %s}" % (c_str(c), c_str(n), c_str(t).replace("\n", "\\n")) for c, n, t in presets)
    return """// generated by surface/surface.py from presets/Factory/*/*.pfp: do not edit
#pragma once

namespace pf {

struct FactoryPreset { const char* category; const char* name; const char* text; };
static const FactoryPreset kFactoryPresets[] = {
%s
};
constexpr int kNumFactoryPresets = %d;

} // namespace pf
""" % (rows, len(presets))


def main():
    keys = [p["key"] for p in P]
    assert len(set(keys)) == len(keys), "duplicate parameter key"
    assert P[0]["kind"] == "readout", "parameter 0 must stay a read-only readout"
    layout = pages()
    check_layout(layout, build_layout().groups)
    presets = factory_presets()   # everything checked before anything is written
    outputs = [
        ("params.json", json.dumps(params_json(), indent=1)),
        ("layout.conf", layout),
        ("vst.json", json.dumps(VST, indent=1)),
        (os.path.join("build", "skin_style.json"), json.dumps(skin_style(), indent=1)),
        (os.path.join("build", "factory_presets.h"), presets_header(presets)),
        (os.path.join("build", "param_ids.h"), header()),   # last: make's target, newer than the rest
    ]
    os.makedirs(os.path.join(HERE, "build"), exist_ok=True)
    for name, text in outputs:
        path = os.path.join(HERE, name)
        with open(path + ".tmp", "w", newline="\n") as f:
            f.write(text)
        os.replace(path + ".tmp", path)
    print("surface: %d parameters, layout ok, %d factory presets" % (len(P), len(presets)))


if __name__ == "__main__":
    sys.exit(main())
