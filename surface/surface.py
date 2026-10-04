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

Continuous parameters are declared to MPC as 0..1: the real range and curve (log Hz, log
seconds, ...) live in param_ids.h, and the plugin formats every value text itself, so the
knob, its label and the DSP can never disagree.

Before writing anything the layout is checked the way shadow_skin.py would (unknown keys,
option counts, when=, Q-Link sets) plus geometry (inside the plugin area, no overlaps within
a page or mode panel), so a broken page fails here instead of on the device.

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
#   ui       a stepped choice the surface uses (page selector, browser target): not saved
#   readout  text the plugin writes (status line, "PAGE 2 / 4"): read only
#   stepper  plugin-owned index into a list (tables, presets), text = the item; moves one
#            item per Q-Link/wheel event; comes with <key>_prev / <key>_next buttons
#   button   momentary: acts on the press, springs back to 0
#   tile     a browser tile (list widget): lit = 1, text = the item; a tap acts
#   toggle   plugin-owned on/off (lit state follows the plugin), a tap acts
#   popup    the hidden "<key>__open" flag of a popup list (shadow_skin's popup_params)
# fmt: how the plugin prints the value (see plugin/patch_map.cpp paramDisplay)
P = []


def _add(key, name, kind, curve, lo, hi, default, fmt, **extra):
    d = dict(key=key, name=name, kind=kind, curve=curve, lo=lo, hi=hi, default=default, fmt=fmt)
    d.update(extra)
    P.append(d)


def readout(key, name):
    _add(key, name, "readout", "readout", 0, 0, 0, "none")


def enum(key, name, options, default, ui=False):
    _add(key, name, "ui" if ui else "synth", "enum", 0, len(options) - 1, options.index(default), "enum",
         options=options)


def num(key, name, curve, lo, hi, default, fmt):
    _add(key, name, "synth", curve, lo, hi, default, fmt)


def stepper(key, name):
    _add(key, name, "stepper", "int", 0, STEPPER_RANGE, 0, "text")
    button(key + "_prev", name + " prev")
    button(key + "_next", name + " next")


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
enum("routing", "Routing", ROUTING, "Serial")

OSC_WAVES = ["Table", "Sine", "Triangle", "Saw", "Square", "Pulse", "Noise"]   # dsp/synth.h OscWave
PHASE_MODES = ["Reset", "Random", "Free"]
ROUTES = ["F1", "F2", "F1+F2", "Direct"]
SUB_WAVES = ["Sine", "Triangle", "Saw", "Square"]

for o, (pos, semi, fine, level) in ((1, (0.66, 0, 0, 0.8)), (2, (0.66, 0, 7, 0.6))):
    enum("o%d_wave" % o, "Osc %d wave" % o, OSC_WAVES, "Table")
    popup_flag("o%d_wave" % o)
    stepper("o%d_table" % o, "Osc %d table" % o)
    num("o%d_pos" % o, "Osc %d position" % o, "lin", 0, 1, pos, "frame%d" % o)
    num("o%d_oct" % o, "Osc %d octave" % o, "int", -3, 3, 0, "oct")
    num("o%d_semi" % o, "Osc %d semitone" % o, "int", -12, 12, semi, "semi")
    num("o%d_fine" % o, "Osc %d fine" % o, "lin", -100, 100, fine, "cent")
    num("o%d_uni" % o, "Osc %d unison" % o, "int", 1, MAX_UNISON, 1, "count")
    num("o%d_detune" % o, "Osc %d detune" % o, "lin", 0, 1, 0.3, "detune")
    num("o%d_width" % o, "Osc %d width" % o, "lin", 0, 1, 0.5, "pct")
    num("o%d_level" % o, "Osc %d level" % o, "lin", 0, 1, level, "pct")
    num("o%d_pan" % o, "Osc %d pan" % o, "lin", -1, 1, 0, "pan")
    num("o%d_phase" % o, "Osc %d phase" % o, "lin", 0, 1, 0, "deg")
    enum("o%d_phmode" % o, "Osc %d phase mode" % o, PHASE_MODES, "Reset")
    enum("o%d_route" % o, "Osc %d route" % o, ROUTES, "F1")
    enum("o%d_sub_wave" % o, "Sub %d wave" % o, SUB_WAVES, "Sine")
    num("o%d_sub_tune" % o, "Sub %d tune" % o, "int", -36, 12, -12, "semi")
    num("o%d_sub_level" % o, "Sub %d level" % o, "lin", 0, 1, 0, "pct")

num("noise_level", "Noise level", "lin", 0, 1, 0, "pct")
num("noise_color", "Noise colour", "lin", -1, 1, 0, "bipct")
enum("noise_route", "Noise route", ROUTES, "F1")
enum("ui_osc", "Oscillator page", ["OSC 1", "OSC 2", "NOISE"], "OSC 1", ui=True)

enum("engine", "Engine", ENGINES, "Normal")
for f, (ftype, cut, env) in ((1, ("LP24", 1200, 0.25)), (2, ("Off", 8000, 0.0))):
    enum("f%d_type" % f, "Filter %d type" % f, FILTER_TYPES, ftype)
    popup_flag("f%d_type" % f)
    num("f%d_cut" % f, "Filter %d cutoff" % f, "log", 20, 20000, cut, "hz")
    num("f%d_res" % f, "Filter %d resonance" % f, "lin", 0, 1, 0.25, "pct")
    num("f%d_env" % f, "Filter %d env 2" % f, "lin", -1, 1, env, "bipct")
    num("f%d_key" % f, "Filter %d keytrack" % f, "lin", 0, 1, 0.5, "pct")
    num("f%d_drive" % f, "Filter %d drive" % f, "lin", 0, 1, 0.0, "pct")

for e, (a, d, s, r) in ((1, (0.003, 0.4, 0.8, 0.3)), (2, (0.001, 0.6, 0.2, 0.5))):
    num("e%d_a" % e, "Env %d attack" % e, "log", 0.001, 20, a, "time")
    num("e%d_d" % e, "Env %d decay" % e, "log", 0.001, 20, d, "time")
    num("e%d_s" % e, "Env %d sustain" % e, "lin", 0, 1, s, "pct")
    num("e%d_r" % e, "Env %d release" % e, "log", 0.001, 20, r, "time")
    if e == 1:
        num("e1_vel", "Env 1 velocity", "lin", 0, 1, 0.5, "pct")
    else:
        num("e2_pos", "Env 2 > wave pos", "lin", -1, 1, 0.0, "bipct")
        num("e2_vel", "Env 2 velocity", "lin", 0, 1, 0.0, "pct")
        enum("e2_loop", "Env 2 loop", ["Off", "Loop"], "Off")

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
    enum(p + "wave", "LFO %d wave" % l, LFO_WAVES, "Sine")
    popup_flag(p + "wave")
    enum(p + "sync", "LFO %d sync" % l, ["Free", "Sync"], "Free")
    num(p + "rate", "LFO %d rate" % l, "log", 0.02, 40, 2.0 if l == 1 else 0.5, "lfohz")
    enum(p + "div", "LFO %d sync rate" % l, SYNC_DIVS, "1/4")
    popup_flag(p + "div")
    num(p + "phase", "LFO %d phase" % l, "lin", 0, 1, 0, "deg")
    num(p + "delay", "LFO %d delay" % l, "pow", 0, 10, 0, "time")
    num(p + "fade", "LFO %d fade in" % l, "pow", 0, 10, 0, "time")
    enum(p + "trig", "LFO %d trigger" % l, ["Retrig", "Free", "Global"], "Retrig")
    enum(p + "polar", "LFO %d polarity" % l, ["Bipolar", "Unipolar"], "Bipolar")
    num(p + "depth", "LFO %d depth" % l, "lin", 0, 1, 1, "pct")

for k in range(1, MOD_SLOTS + 1):
    p = "m%d_" % k
    enum(p + "src", "Mod %d source" % k, MOD_SOURCES, "None")
    enum(p + "via", "Mod %d via" % k, MOD_SOURCES, "None")
    enum(p + "mod", "Mod %d modifier" % k, MODIFIERS, "None")
    num(p + "modamt", "Mod %d modifier amount" % k, "lin", -1, 1, 0, "bipct")
    enum(p + "t1", "Mod %d target 1" % k, MOD_TARGETS, "Off")   # each amount right after its target
    num(p + "a1", "Mod %d amount 1" % k, "lin", -1, 1, 0, "modamt")
    enum(p + "t2", "Mod %d target 2" % k, MOD_TARGETS, "Off")
    num(p + "a2", "Mod %d amount 2" % k, "lin", -1, 1, 0, "modamt")
    for key in ("src", "via", "mod", "t1", "t2"):
        popup_flag(p + key)

# --- sequencing (Milestone 6): arpeggiator, 16-step sequencer, 4 x 8-step shape sequencer ---
ARP_DIRS = ["Up", "Down", "Up/Down", "Down/Up", "Played", "Random", "Chord"]   # dsp/notegen.h ArpDir
SEQ_STEPS = 16
SHAPE_STEPS = 8
enum("seq_mode", "Arp/Seq", ["Off", "Arp", "Seq"], "Off")
enum("arp_dir", "Arp direction", ARP_DIRS, "Up")
popup_flag("arp_dir")
num("arp_oct", "Arp octaves", "int", 1, 4, 1, "count")
enum("clk_rate", "Arp/Seq rate", SYNC_DIVS, "1/16")
popup_flag("clk_rate")
num("clk_gate", "Arp/Seq gate", "lin", 0.05, 1, 0.5, "pct")
num("clk_swing", "Arp/Seq swing", "lin", 0, 0.5, 0, "pct")
enum("arp_latch", "Arp latch", ["Off", "Latch"], "Off")
enum("arp_pattern", "Arp pattern", ["Off", "Steps"], "Off")
num("seq_steps", "Seq steps", "int", 1, SEQ_STEPS, SEQ_STEPS, "count")
enum("seq_rec", "Seq record", ["Off", "Rec"], "Off", ui=True)
for k in range(1, SEQ_STEPS + 1):
    num("s%d_note" % k, "Step %d note" % k, "int", -24, 24, 0, "semi")
for k in range(1, SEQ_STEPS + 1):
    num("s%d_vel" % k, "Step %d velocity" % k, "int", 0, 127, 100, "count")
for k in range(1, SEQ_STEPS + 1):
    num("s%d_mod" % k, "Step %d mod" % k, "lin", -1, 1, 0, "bipct")
enum("sh_rate", "Shape rate", SYNC_DIVS, "1/8")
popup_flag("sh_rate")
num("sh_steps", "Shape steps", "int", 1, SHAPE_STEPS, SHAPE_STEPS, "count")
for l in range(1, 5):
    enum("sh%d_mode" % l, "Shape %d mode" % l, ["Step", "Ramp", "Smooth"], "Step")
    for k in range(1, SHAPE_STEPS + 1):
        num("sh%d_%d" % (l, k), "Shape %d step %d" % (l, k), "lin", -1, 1, 0, "bipct")
enum("ui_seq", "Sequencer page", ["ARP", "STEPS", "SHAPES"], "ARP", ui=True)

for x in range(1, 5):
    num("xy%d_x" % x, "XY %d X" % x, "lin", 0, 1, 0, "pct")
    num("xy%d_y" % x, "XY %d Y" % x, "lin", 0, 1, 0, "pct")
button("xy_auto", "XY auto-assign")
enum("ui_mod", "Modulation page", ["ENVELOPES", "LFO 1", "LFO 2", "XY"], "ENVELOPES", ui=True)
enum("ui_mx", "Matrix page", ["1-4", "5-8", "9-12", "MODIFIERS"], "1-4", ui=True)

# --- voices (Milestone 2) ---
enum("vmode", "Voice mode", ["Poly", "Duo", "Mono", "Legato"], "Poly")
enum("steal", "Steal", ["Oldest", "Quietest", "Keep low", "Keep high"], "Oldest")
enum("same_note", "Same note", ["Retrigger", "New voice"], "Retrigger")
enum("glide_mode", "Glide", ["Off", "Always", "Legato"], "Off")
enum("glide_type", "Glide type", ["Time", "Rate"], "Time")
num("glide", "Glide time", "log", 0.001, 10, 0.08, "time")
num("bend_up", "Bend up", "int", 0, 24, 2, "semi")
num("bend_dn", "Bend down", "int", 0, 24, 2, "semi")
num("vel_curve", "Velocity curve", "lin", -1, 1, 0, "bipct")

# --- the wavetable browser (docs/M1_DESIGN.md §6.2) ---
enum("br_target", "Browse for", ["OSC 1", "OSC 2"], "OSC 1", ui=True)
for i in range(1, BROWSER_CATS + 1):
    tile("cat_%d" % i, "Category %d" % i)
button("cat_prev", "Categories prev")
button("cat_next", "Categories next")
for i in range(1, BROWSER_ITEMS + 1):
    tile("tbl_%d" % i, "Table %d" % i)
button("tbl_prev", "Tables prev")
button("tbl_next", "Tables next")
readout("tbl_page", "Tables page")
readout("br_now", "Loaded")
toggle("fav", "Favorite")
button("rnd", "Random")
button("copy", "Copy 1 > 2")
button("swap", "Swap 1 <> 2")


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
# Plugin area 1280x628 at y = 86..714. Every page: status line on top, two 270 px rows below.
# A knob (r=30) spans cy-35 .. cy+93 and is 130 px wide; 8 slots per row like the Force's knobs.
THEME = """style=default
font_label=fonts/TitilliumWeb-SemiBold.ttf
title_size=19
theme_title=7a6a52
theme_bg=111012
theme_ink=ece8e1
theme_ink_dim=c2b9aa
theme_accent=e8a33d
theme_accent_hi=f6cf8a
theme_line=2c2924
theme_lcd=15120d
theme_tile_on=3a2c14
theme_box=1a1815
theme_btn_bg=24211c
theme_btn_text=ece8e1
theme_seg_active=e8a33d
theme_seg_inactive=1a1815
theme_seg_active_tx=140e04
theme_display_ink=f2c879
"""
SLOT8 = [110, 261, 412, 563, 714, 865, 1016, 1167]
ROW_Y = (156, 440)           # frame tops
ROW_ENUM = 44                # enum centre below the frame top
ROW_KNOB = 134               # knob centre below the frame top
R = 30


def knob(cx, cy, label, key, when=None):
    return 'knob cx=%d cy=%d r=%d label="%s" key=%s%s' % (cx, cy, R, label, key, _when(when))


def _when(when):
    return " when=%s" % when if when else ""


def pages():
    L = [THEME]
    status = 'readout cx=640 cy=126 w=1212 h=40 key=status'

    # OSC: one oscillator (or the noise source) at a time, picked top right. Row 1: wave,
    # table and the 8 knobs (Q-Links 1-8); row 2: pan, phase, routing and the sub oscillator.
    L.append("[tab OSC]")
    L.append('readout cx=440 cy=126 w=812 h=40 key=status')
    L.append('enum_h cx=1080 cy=126 sw=110 key=ui_osc')
    osc_knobs = [("pos", "POSITION"), ("oct", "OCTAVE"), ("semi", "SEMI"), ("fine", "FINE"),
                 ("uni", "UNISON"), ("detune", "DETUNE"), ("width", "WIDTH"), ("level", "LEVEL")]
    for o in (1, 2):
        w = "ui_osc:OSC %d" % o
        p = "o%d_" % o
        L.append('frame x=34 y=156 w=1212 h=270 title="OSCILLATOR %d" when="%s"' % (o, w))
        L.append('popup cx=150 cy=200 w=200 h=44 key=%swave when="%s"' % (p, w))
        L.append('stepper cx=760 cy=200 w=720 h=44 key=%stable when="%s"' % (p, w))
        for cx, (k, lab) in zip(SLOT8, osc_knobs):
            L.append(knob(cx, ROW_Y[0] + ROW_KNOB, lab, p + k, '"%s"' % w))
        L.append('frame x=34 y=440 w=1212 h=270 title="PAN  PHASE  ROUTE  SUB" when="%s"' % w)
        L.append(knob(SLOT8[0], ROW_Y[1] + ROW_KNOB, "PAN", p + "pan", '"%s"' % w))
        L.append(knob(SLOT8[1], ROW_Y[1] + ROW_KNOB, "PHASE", p + "phase", '"%s"' % w))
        L.append('text cx=400 cy=500 label="PHASE" when="%s"' % w)
        L.append('enum_v cx=400 cy=578 sw=140 key=%sphmode when="%s"' % (p, w))
        L.append('text cx=580 cy=500 label="ROUTE" when="%s"' % w)
        L.append('enum_v cx=580 cy=594 sw=140 key=%sroute when="%s"' % (p, w))
        L.append('text cx=790 cy=500 label="SUB WAVE" when="%s"' % w)
        L.append('enum_v cx=790 cy=594 sw=150 key=%ssub_wave when="%s"' % (p, w))
        L.append(knob(SLOT8[6], ROW_Y[1] + ROW_KNOB, "SUB TUNE", p + "sub_tune", '"%s"' % w))
        L.append(knob(SLOT8[7], ROW_Y[1] + ROW_KNOB, "SUB LEVEL", p + "sub_level", '"%s"' % w))
    w = '"ui_osc:NOISE"'
    L.append('frame x=34 y=156 w=600 h=270 title="NOISE" when=%s' % w)
    L.append(knob(SLOT8[0], ROW_Y[0] + ROW_KNOB, "LEVEL", "noise_level", w))
    L.append(knob(SLOT8[1], ROW_Y[0] + ROW_KNOB, "COLOUR", "noise_color", w))
    L.append('text cx=480 cy=216 label="ROUTE" when=%s' % w)
    L.append('enum_v cx=480 cy=310 sw=140 key=noise_route when=%s' % w)
    L.append('frame x=646 y=156 w=600 h=270 title="SUB LEVELS" when=%s' % w)
    L.append(knob(SLOT8[4] + 2, ROW_Y[0] + ROW_KNOB, "SUB 1", "o1_sub_level", w))
    L.append(knob(SLOT8[5] + 2, ROW_Y[0] + ROW_KNOB, "SUB 2", "o2_sub_level", w))
    L.append(knob(SLOT8[6] + 2, ROW_Y[0] + ROW_KNOB, "OSC 1", "o1_level", w))
    L.append(knob(SLOT8[7] + 2, ROW_Y[0] + ROW_KNOB, "OSC 2", "o2_level", w))
    for o in (1, 2):
        p = "o%d_" % o
        L.append('qlinks "OSC %d" = ' % o + ",".join(p + k for k in (
            "pos", "oct", "semi", "fine", "uni", "detune", "width", "level",
            "wave", "table", "pan", "phase", "phmode", "route", "sub_tune", "sub_level")))
    L.append('qlinks "NOISE" = ' + ",".join(
        ["noise_level", "noise_color", "noise_route", "o1_sub_level", "o2_sub_level", "o1_level", "o2_level", "volume",
         "o1_sub_tune", "o2_sub_tune", "o1_sub_wave", "o2_sub_wave", "o1_pos", "o2_pos", "f1_cut", "f2_cut"]))

    # FILTER: type selector + 5 knobs per filter, routing next to the status line
    flt_knobs = [("cut", "CUTOFF"), ("res", "RESO"), ("env", "ENV 2"), ("key", "KEYTRACK"), ("drive", "DRIVE")]
    L.append("[tab FILTER]")
    L.append('readout cx=330 cy=126 w=592 h=40 key=status')
    L.append('enum_h cx=780 cy=126 sw=120 key=routing')
    L.append('enum_h cx=1112 cy=126 sw=110 key=engine')
    for f, top in zip((1, 2), ROW_Y):
        L.append('frame x=34 y=%d w=1212 h=270 title="FILTER %d"' % (top, f))
        L.append('popup cx=1040 cy=%d w=220 h=44 key=f%d_type' % (top + ROW_ENUM, f))
        for cx, (k, lab) in zip(SLOT8, flt_knobs):
            L.append(knob(cx, top + ROW_KNOB, lab, "f%d_%s" % (f, k)))
    L.append('qlinks "FILTER" = ' + ",".join(
        ["f1_cut", "f1_res", "f1_env", "f1_drive", "f2_cut", "f2_res", "f2_env", "f2_drive",
         "f1_key", "f2_key", "f1_type", "f2_type", "routing", "engine", "e2_pos", "volume"]))

    # MOD: the envelopes, the two LFOs and the XY pads, one panel at a time.
    L.append("[tab MOD]")
    L.append('readout cx=420 cy=126 w=772 h=40 key=status')
    L.append('enum_h cx=1040 cy=126 sw=100 key=ui_mod')
    w = '"ui_mod:ENVELOPES"'
    L.append('frame x=34 y=156 w=1212 h=270 title="ENV 1   (AMP)" when=%s' % w)
    for cx, (k, lab) in zip(SLOT8, [("a", "ATTACK"), ("d", "DECAY"), ("s", "SUSTAIN"), ("r", "RELEASE"), ("vel", "VELOCITY")]):
        L.append(knob(cx, ROW_Y[0] + ROW_KNOB, lab, "e1_" + k, w))
    L.append('frame x=34 y=440 w=1212 h=270 title="ENV 2   (MOD)" when=%s' % w)
    for cx, (k, lab) in zip(SLOT8, [("a", "ATTACK"), ("d", "DECAY"), ("s", "SUSTAIN"), ("r", "RELEASE"),
                                    ("vel", "VELOCITY"), ("pos", "> WAVE POS")]):
        L.append(knob(cx, ROW_Y[1] + ROW_KNOB, lab, "e2_" + k, w))
    L.append('text cx=1100 cy=516 label="LOOP" when=%s' % w)
    L.append('enum_v cx=1100 cy=568 sw=140 key=e2_loop when=%s' % w)
    for l in (1, 2):
        w = '"ui_mod:LFO %d"' % l
        p = "l%d_" % l
        L.append('frame x=34 y=156 w=1212 h=270 title="LFO %d" when=%s' % (l, w))
        L.append('popup cx=150 cy=200 w=200 h=44 key=%swave when=%s' % (p, w))
        L.append('enum_h cx=420 cy=200 sw=100 key=%ssync when=%s' % (p, w))
        L.append('popup cx=640 cy=200 w=150 h=44 key=%sdiv when=%s' % (p, w))
        L.append('enum_h cx=920 cy=200 sw=120 key=%spolar when=%s' % (p, w))
        for cx, (k, lab) in zip(SLOT8, [("rate", "RATE"), ("phase", "PHASE"), ("delay", "DELAY"), ("fade", "FADE IN"),
                                        ("depth", "DEPTH")]):
            L.append(knob(cx, ROW_Y[0] + ROW_KNOB, lab, p + k, w))
        L.append('frame x=34 y=440 w=1212 h=270 title="TRIGGER" when=%s' % w)
        L.append('enum_h cx=330 cy=500 sw=140 key=%strig when=%s' % (p, w))
        L.append('text cx=640 cy=600 label="RETRIG: EVERY NOTE  FREE: PER VOICE  GLOBAL: ONE FOR ALL, SYNCED TO THE BAR" when=%s' % w)
    w = '"ui_mod:XY"'
    L.append('frame x=34 y=156 w=1212 h=270 title="XY PADS" when=%s' % w)
    for x in range(1, 5):
        L.append(knob(SLOT8[2 * x - 2], ROW_Y[0] + ROW_KNOB, "X%d" % x, "xy%d_x" % x, w))
        L.append(knob(SLOT8[2 * x - 1], ROW_Y[0] + ROW_KNOB, "Y%d" % x, "xy%d_y" % x, w))
    L.append('frame x=34 y=440 w=1212 h=270 title="ASSIGN" when=%s' % w)
    L.append('button cx=200 cy=520 label="AUTO-ASSIGN" key=xy_auto when=%s' % w)
    L.append('text cx=760 cy=520 label="FILLS FREE MATRIX SLOTS: X1/Y1 CUTOFF/RESO, X2/Y2 WAVE POSITIONS ..." when=%s' % w)
    L.append('qlinks "ENVELOPES" = ' + ",".join(
        ["e1_a", "e1_d", "e1_s", "e1_r", "e2_a", "e2_d", "e2_s", "e2_r",
         "e1_vel", "e2_vel", "e2_pos", "e2_loop", "volume", "voices", "f1_env", "f2_env"]))
    for l in (1, 2):
        p = "l%d_" % l
        L.append('qlinks "LFO %d" = ' % l + ",".join(p + k for k in (
            "rate", "phase", "delay", "fade", "depth", "wave", "sync", "div", "trig", "polar")))
    L.append('qlinks "XY" = ' + ",".join("xy%d_%s" % (x, a) for x in range(1, 5) for a in "xy"))

    # MATRIX: 12 slots, four per panel (source, via, two targets with amounts); the modifiers
    # of all 12 on their own panel.
    L.append("[tab MATRIX]")
    L.append('readout cx=420 cy=126 w=772 h=40 key=status')
    L.append('enum_h cx=1040 cy=126 sw=100 key=ui_mx')
    rows = [230 + 125 * r for r in range(4)]
    cols = [("src", "SOURCE", 180, 180), ("via", "VIA", 375, 170), ("t1", "TARGET 1", 590, 220),
            ("a1", "AMOUNT", 785, 0), ("t2", "TARGET 2", 975, 220), ("a2", "AMOUNT", 1170, 0)]
    for page, name in enumerate(["1-4", "5-8", "9-12"]):
        w = '"ui_mx:%s"' % name
        L.append('frame x=34 y=156 w=1212 h=554 title="SLOTS %s" when=%s' % (name, w))
        for key, lab, cx, width in cols:
            L.append('text cx=%d cy=186 label="%s" when=%s' % (cx, lab, w))
        for r, cy in enumerate(rows):
            k = page * 4 + r + 1
            p = "m%d_" % k
            L.append('text cx=58 cy=%d label="%d" when=%s' % (cy, k, w))
            for key, lab, cx, width in cols:
                if width:
                    L.append('popup cx=%d cy=%d w=%d h=40 key=%s%s when=%s' % (cx, cy, width, p, key, w))
                else:
                    L.append('knob cx=%d cy=%d r=22 label="%s" key=%s%s when=%s' % (cx, cy, lab, p, key, w))
        L.append('qlinks "SLOTS %s" = ' % name + ",".join(
            ["m%d_%s" % (page * 4 + r + 1, a) for r in range(4) for a in ("a1", "a2")] +
            ["m%d_src" % (page * 4 + r + 1) for r in range(4)] + ["m%d_t1" % (page * 4 + r + 1) for r in range(4)]))
    w = '"ui_mx:MODIFIERS"'
    L.append('frame x=34 y=156 w=1212 h=554 title="MODIFIERS" when=%s' % w)
    for k in range(1, MOD_SLOTS + 1):
        c, r = (k - 1) // 4, (k - 1) % 4
        x0 = 34 + c * 404
        p = "m%d_" % k
        L.append('text cx=%d cy=%d label="%d" when=%s' % (x0 + 24, rows[r], k, w))
        L.append('popup cx=%d cy=%d w=160 h=40 key=%smod when=%s' % (x0 + 130, rows[r], p, w))
        L.append('knob cx=%d cy=%d r=22 label="AMOUNT" key=%smodamt when=%s' % (x0 + 290, rows[r], p, w))
    L.append('qlinks "MODIFIERS" = ' + ",".join("m%d_modamt" % k for k in range(1, MOD_SLOTS + 1)))

    # VOICE: how notes become voices (Milestone 2)
    L.append("[tab VOICE]")
    L.append(status)
    L.append('frame x=34 y=156 w=1212 h=270 title="VOICES"')
    L.append('enum_h cx=420 cy=200 sw=130 key=vmode')
    L.append(knob(SLOT8[0], ROW_Y[0] + ROW_KNOB, "VOICES", "voices"))
    L.append(knob(SLOT8[1], ROW_Y[0] + ROW_KNOB, "VEL CURVE", "vel_curve"))
    L.append(knob(SLOT8[2], ROW_Y[0] + ROW_KNOB, "VOLUME", "volume"))
    L.append('text cx=780 cy=232 label="STEAL"')
    L.append('enum_v cx=780 cy=318 sw=170 key=steal')
    L.append('text cx=1060 cy=232 label="SAME NOTE"')
    L.append('enum_v cx=1060 cy=286 sw=170 key=same_note')
    L.append('frame x=34 y=440 w=1212 h=270 title="GLIDE + BEND"')
    L.append(knob(SLOT8[0], ROW_Y[1] + ROW_KNOB, "GLIDE", "glide"))
    L.append('text cx=380 cy=516 label="GLIDE"')
    L.append('enum_v cx=380 cy=584 sw=150 key=glide_mode')
    L.append('text cx=600 cy=516 label="TYPE"')
    L.append('enum_v cx=600 cy=568 sw=150 key=glide_type')
    L.append(knob(SLOT8[5], ROW_Y[1] + ROW_KNOB, "BEND UP", "bend_up"))
    L.append(knob(SLOT8[6], ROW_Y[1] + ROW_KNOB, "BEND DOWN", "bend_dn"))
    L.append('qlinks "VOICE" = ' + ",".join(
        ["voices", "vel_curve", "volume", "vmode", "steal", "same_note", "glide_mode", "glide_type",
         "glide", "bend_up", "bend_dn", "o1_pos", "o2_pos", "f1_cut", "f2_cut", "e2_pos"]))

    # SEQ: arpeggiator / step sequencer settings, the 16 steps, the 4 shape lanes.
    L.append("[tab SEQ]")
    L.append('readout cx=420 cy=126 w=772 h=40 key=status')
    L.append('enum_h cx=1060 cy=126 sw=110 key=ui_seq')
    w = '"ui_seq:ARP"'
    L.append('frame x=34 y=156 w=1212 h=270 title="ARP / SEQUENCER" when=%s' % w)
    L.append('enum_h cx=220 cy=200 sw=110 key=seq_mode when=%s' % w)
    L.append('popup cx=560 cy=200 w=180 h=44 key=arp_dir when=%s' % w)
    L.append('popup cx=790 cy=200 w=150 h=44 key=clk_rate when=%s' % w)
    L.append('enum_h cx=1080 cy=200 sw=110 key=arp_latch when=%s' % w)
    for cx, (k, lab) in zip(SLOT8, [("arp_oct", "OCTAVES"), ("clk_gate", "GATE"), ("clk_swing", "SWING"),
                                    ("seq_steps", "STEPS")]):
        L.append(knob(cx, ROW_Y[0] + ROW_KNOB, lab, k, w))
    L.append('text cx=780 cy=262 label="ARP PATTERN" when=%s' % w)
    L.append('enum_v cx=780 cy=314 sw=150 key=arp_pattern when=%s' % w)
    L.append('text cx=1060 cy=262 label="RECORD STEPS" when=%s' % w)
    L.append('enum_v cx=1060 cy=314 sw=150 key=seq_rec when=%s' % w)
    L.append('frame x=34 y=440 w=1212 h=270 title="SHAPE SEQUENCER CLOCK" when=%s' % w)
    L.append('popup cx=200 cy=500 w=160 h=44 key=sh_rate when=%s' % w)
    L.append(knob(SLOT8[3], ROW_Y[1] + ROW_KNOB, "SHAPE STEPS", "sh_steps", w))
    L.append('text cx=840 cy=600 label="SHAPE 1-4 AND SEQ ARE SOURCES IN THE MATRIX" when=%s' % w)
    w = '"ui_seq:STEPS"'
    for r, (key, title, cy) in enumerate([("note", "NOTE  (ST FROM THE KEY)", 224), ("vel", "VELOCITY  (0 = REST)", 410),
                                          ("mod", "MOD  (THE SEQ SOURCE)", 596)]):
        L.append('frame x=34 y=%d w=1212 h=180 title="%s" when=%s' % (156 + 186 * r, title, w))
        for k in range(1, SEQ_STEPS + 1):
            L.append('slider_v cx=%d cy=%d w=18 h=84 cw=72 label="%d" key=s%d_%s when=%s' % (
                71 + (k - 1) * 74, cy, k, k, key, w))
    w = '"ui_seq:SHAPES"'
    for l in range(1, 5):
        top = 156 + 140 * (l - 1)
        L.append('frame x=34 y=%d w=1212 h=134 title="SHAPE %d" when=%s' % (top, l, w))
        L.append('enum_v cx=150 cy=%d sw=150 key=sh%d_mode when=%s' % (top + 78, l, w))
        for k in range(1, SHAPE_STEPS + 1):
            L.append('slider_v cx=%d cy=%d w=16 h=50 cw=100 label="%d" key=sh%d_%d when=%s' % (
                330 + (k - 1) * 112, top + 52, k, l, k, w))
    L.append('qlinks "ARP" = ' + ",".join(
        ["arp_oct", "clk_gate", "clk_swing", "seq_steps", "seq_mode", "arp_dir", "clk_rate", "arp_latch",
         "arp_pattern", "sh_rate", "sh_steps", "volume", "f1_cut", "f1_res", "o1_pos", "o2_pos"]))
    for key in ("note", "vel", "mod"):
        L.append('qlinks "STEP %s" = ' % key.upper() + ",".join("s%d_%s" % (k, key) for k in range(1, SEQ_STEPS + 1)))
    L.append('qlinks "SHAPE 1+2" = ' + ",".join("sh%d_%d" % (l, k) for l in (1, 2) for k in range(1, SHAPE_STEPS + 1)))
    L.append('qlinks "SHAPE 3+4" = ' + ",".join("sh%d_%d" % (l, k) for l in (3, 4) for k in range(1, SHAPE_STEPS + 1)))

    # TABLES: the browser (docs/M1_DESIGN.md §6.2). Categories left, tables right, actions below.
    L.append("[tab TABLES]")
    L.append('readout cx=440 cy=126 w=812 h=40 key=status')
    L.append('enum_h cx=1080 cy=126 sw=150 key=br_target')
    L.append('frame x=34 y=156 w=360 h=554 title="CATEGORY"')
    L.append('list x=46 y=196 w=336 cols=2 rows=8 th=48 gap=8 key=cat')
    L.append('button cx=130 cy=672 label="< PREV" key=cat_prev')
    L.append('button cx=298 cy=672 label="NEXT >" key=cat_next')
    L.append('frame x=406 y=156 w=840 h=470 title="TABLES"')
    L.append('list x=418 y=196 w=816 cols=3 rows=8 th=42 gap=6 key=tbl')
    L.append('button cx=500 cy=600 label="< PREV" key=tbl_prev')
    L.append('readout cx=826 cy=600 w=360 h=34 key=tbl_page')
    L.append('button cx=1152 cy=600 label="NEXT >" key=tbl_next')
    L.append('readout cx=650 cy=672 w=470 h=40 key=br_now')
    L.append('toggle cx=950 cy=664 label="FAV" key=fav')
    L.append('button cx=1060 cy=672 label="RND" key=rnd')
    L.append('button cx=1140 cy=672 label="1>2" key=copy')
    L.append('button cx=1222 cy=672 label="1<>2" key=swap')
    L.append('qlinks "TABLES" = ' + ",".join(
        ["o1_table", "o1_pos", "o1_level", "o1_detune", "o2_table", "o2_pos", "o2_level", "o2_detune",
         "f1_cut", "f1_res", "f1_env", "e2_pos", "f2_cut", "f2_res", "f2_env", "volume"]))
    return "\n".join(L) + "\n"


# --- layout check (offline, no skin toolchain) ---------------------------------------------------
X0, Y0, X1, Y1 = 0, 86, 1280, 714
KNOB_W, KNOB_UP, KNOB_DOWN = 130, 35, 93
TOGGLE_W, TOGGLE_UP, TOGGLE_DOWN = 120, 18, 40


def _widget(line):
    toks = shlex.split(line)
    w = {"kind": toks[0]}
    for t in toks[1:]:
        k, _, v = t.partition("=")
        w[k] = int(v) if k in ("x", "y", "w", "h", "cx", "cy", "r", "sw", "rows", "cols", "th", "gap", "cw") else v
    return w


def _rects(w, params):
    """Touch/drawn rectangles of a widget (x, y, w, h), as shadow_skin.py places them."""
    k = w["kind"]
    if k == "knob":   # shadow_skin.py: filmstrip 2r+10 square, name and value labels under it
        r = w.get("r", R)
        side = 2 * r + 10
        height = side // 2 + r + 2 + 20 + 2 + 26 + 6
        return [(w["cx"] - max(130, side) // 2, w["cy"] - side // 2, max(130, side), height)]
    if k == "toggle":
        return [(w["cx"] - TOGGLE_W // 2, w["cy"] - TOGGLE_UP, TOGGLE_W, TOGGLE_UP + TOGGLE_DOWN)]
    if k == "button":
        bw = 11 * len(w.get("label", "")) + 36   # flat estimate, wider than the real font
        return [(w["cx"] - bw // 2, w["cy"] - 20, bw, 39)]
    if k in ("readout", "stepper", "popup"):
        return [(w["cx"] - w["w"] // 2, w["cy"] - w["h"] // 2, w["w"], w["h"])]
    if k == "list":
        return [(w["x"], w["y"], w["w"], w["rows"] * w["th"] + (w["rows"] - 1) * w["gap"])]
    if k in ("enum_h", "enum_v"):
        n = len(params[w["key"]]["options"])
        if k == "enum_v":
            sw = w.get("sw") or 135
            return [(w["cx"] - sw // 2, w["cy"] - (n * 32) // 2, sw, n * 32)]
        sw = w.get("sw") or 117
        rows = w.get("rows", 1)
        per = -(-n // rows)
        return [(w["cx"] - (per * sw + (per - 1) * 2) // 2, w["cy"] - 16, per * sw + (per - 1) * 2, rows * 35 - 2)]
    if k in ("slider_v", "slider_h"):
        cw = w.get("cw") or max(130, w["w"], w["h"])
        sq = max(w["w"], w["h"])
        return [(w["cx"] - cw // 2, w["cy"] - sq // 2, cw, sq + 56)]
    return []


def _overlap(a, b):
    return a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and a[1] < b[1] + b[3] and b[1] < a[1] + a[3]


def check_layout(text):
    """Raise SystemExit on anything shadow_skin.py would refuse, plus geometry mistakes."""
    params = {p["key"]: p for p in P}
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
    if len(tabs) > 7:
        errors.append("%d tabs: MPC shows five plus a pager; keep it to seven" % len(tabs))
    for tab in tabs:
        placed = []
        for w in tab["widgets"]:
            kind = w["kind"]
            if kind in ("frame", "text", "art"):
                continue
            key = w.get("key")
            need = ["%s_%d" % (key, i + 1) for i in range(w["cols"] * w["rows"])] if kind == "list" else [key]
            if kind == "stepper":
                need += [key + "_prev", key + "_next"]
            if kind == "popup":
                need.append(key + "__open")
            for k in need:
                if k not in params:
                    errors.append("%s: %s key %r is not a parameter" % (tab["name"], kind, k))
            if any(k not in params for k in need):
                continue
            p = params[key] if kind != "list" else params[need[0]]
            if kind in ("enum_h", "enum_v", "popup") and "options" not in p:
                errors.append("%s: %s %r is not an option parameter" % (tab["name"], kind, key))
            if kind == "list" and p["kind"] != "tile":
                errors.append("%s: list %r tiles must be tile parameters" % (tab["name"], key))
            if kind == "stepper" and p["kind"] != "stepper":
                errors.append("%s: stepper %r is not a stepper parameter" % (tab["name"], key))
            mode = w.get("when")
            if mode:
                mk, _, mo = mode.partition(":")
                opts = [o.lower() for o in params.get(mk, {}).get("options", [])]
                if len(opts) < 2 or mo.lower() not in opts:
                    errors.append("%s: when=%s is not an option of an option parameter" % (tab["name"], mode))
            for r in _rects(w, params):
                if r[0] < X0 or r[1] < Y0 or r[0] + r[2] > X1 or r[1] + r[3] > Y1:
                    errors.append("%s: %s %s at %s leaves the plugin area" % (tab["name"], kind, key, r))
                for other_mode, o_r, o_key in placed:
                    if (mode is None or other_mode is None or mode == other_mode) and _overlap(r, o_r):
                        errors.append("%s: %s overlaps %s" % (tab["name"], key, o_key))
                placed.append((mode, r, key))
        for title, keys in tab["qlinks"]:
            if len(keys) > 16:
                errors.append("%s: qlinks %r has %d keys (max 16)" % (tab["name"], title, len(keys)))
            for k in keys:
                if k not in params:
                    errors.append("%s: qlinks %r key %r is not a parameter" % (tab["name"], title, k))
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
        "tile": "Tile", "toggle": "Toggle", "popup": "Popup"}


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
            opts.append("static const char* const OPTS_%d[] = {%s};" % (i, ", ".join(c_str(o) for o in p["options"])))
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
enum class Kind : unsigned char { Synth, Ui, Readout, Stepper, Button, Tile, Toggle, Popup };

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

static const ParamSpec PARAM_SPECS[P_COUNT] = {
%s
};

%s

static const ParamInfo PARAM_INFO[P_COUNT] = {
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

} // namespace pf
""" % (ids, specs, "\n".join(opts), info, c_str(VST["name"]), c_str(VST["vendor"]), uid, VST["uid"],
       VST["version"], len(FILTER_TYPES), MAX_VOICES, MAX_UNISON, STEPPER_RANGE, BROWSER_CATS, BROWSER_ITEMS,
       len(LFO_WAVES), len(SYNC_DIVS), len(MOD_SOURCES), len(MOD_TARGETS), len(MODIFIERS), MOD_SLOTS,
       len(ARP_DIRS), SEQ_STEPS, SHAPE_STEPS)


def main():
    keys = [p["key"] for p in P]
    assert len(set(keys)) == len(keys), "duplicate parameter key"
    assert P[0]["kind"] == "readout", "parameter 0 must stay a read-only readout"
    layout = pages()
    check_layout(layout)
    os.makedirs(os.path.join(HERE, "build"), exist_ok=True)
    with open(os.path.join(HERE, "params.json"), "w", newline="\n") as f:
        json.dump(params_json(), f, indent=1)
    with open(os.path.join(HERE, "layout.conf"), "w", newline="\n") as f:
        f.write(layout)
    with open(os.path.join(HERE, "vst.json"), "w", newline="\n") as f:
        json.dump(VST, f, indent=1)
    with open(os.path.join(HERE, "build", "param_ids.h"), "w", newline="\n") as f:
        f.write(header())
    print("surface: %d parameters, layout ok" % len(P))


if __name__ == "__main__":
    sys.exit(main())
