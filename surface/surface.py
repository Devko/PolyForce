#!/usr/bin/env python3
"""PolyForce touchscreen surface: the ONE place the plugin's parameters and pages are defined.

Writes, next to this file:
  params.json      ordered VST parameter list (index = position; append-only once released:
                   MPC projects store values by index)
  layout.conf      the skin (shadow_page.conf syntax, see
                   third_party/mpc-vst-plugins/tools/shadow_skin.py); coords are 1280x800
                   Force-Shadow pixels, the plugin area is y = 86..714
  vst.json         plugin identity for the vendored gen_vst.py
  build/param_ids.h  parameter ids + value curves the C++ is compiled against

Continuous parameters are declared to MPC as 0..1: the real range and curve (log Hz, log
seconds, ...) live in param_ids.h, and the plugin formats every value text itself, so the
knob, its label and the DSP can never disagree.

Run: python3 surface/surface.py   (make surface does this before gen_vst.py)
"""
import json
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))

WAVES = ["Classic", "PWM", "Sync", "Formant"]          # dsp/wavetable.cpp builds them in this order
# Polyphony and unison ceilings. They must equal kMaxVoices/kMaxUnison in dsp/synth.h
# (patch_map.cpp static_asserts it). 8 x 8 x 2 oscillators measured 15.5% of a block on
# the Force (2026-10-04); 16 x 16 was 45%.
MAX_VOICES = 8
MAX_UNISON = 8
FILTER_TYPES = ["Off", "LP12", "LP24", "BP", "HP12", "HP24", "Notch", "Peak"]
ROUTING = ["Serial", "Parallel"]

VST = {"name": "PolyForce", "vendor": "Devko", "uid": "PlFc", "version": 1000,
       "so": "polyforce.so", "params": "params.json", "layout": "layout.conf"}


# --- parameters --------------------------------------------------------------------------
# curve: readout | enum | lin | log | int      fmt: how the plugin prints the value
P = []


def readout(key, name):
    P.append(dict(key=key, name=name, curve="readout", lo=0, hi=0, default=0, fmt="none"))


def enum(key, name, options, default):
    P.append(dict(key=key, name=name, curve="enum", lo=0, hi=len(options) - 1,
                  default=options.index(default), fmt="enum", options=options))


def num(key, name, curve, lo, hi, default, fmt):
    P.append(dict(key=key, name=name, curve=curve, lo=lo, hi=hi, default=default, fmt=fmt))


readout("status", "Status")            # index 0 must stay a read-only readout: MPC sets it at load
num("volume", "Volume", "lin", -60, 6, -6, "db")
num("voices", "Voices", "int", 1, MAX_VOICES, MAX_VOICES, "count")
enum("routing", "Routing", ROUTING, "Serial")

for o, (pos, semi, fine, level) in ((1, (0.66, 0, 0, 0.8)), (2, (0.66, 0, 7, 0.6))):
    enum("o%d_wave" % o, "Osc %d wave" % o, WAVES, "Classic")
    num("o%d_pos" % o, "Osc %d position" % o, "lin", 0, 1, pos, "pct")
    num("o%d_oct" % o, "Osc %d octave" % o, "int", -3, 3, 0, "oct")
    num("o%d_semi" % o, "Osc %d semitone" % o, "int", -12, 12, semi, "semi")
    num("o%d_fine" % o, "Osc %d fine" % o, "lin", -100, 100, fine, "cent")
    num("o%d_uni" % o, "Osc %d unison" % o, "int", 1, MAX_UNISON, 1, "count")
    num("o%d_detune" % o, "Osc %d detune" % o, "lin", 0, 1, 0.3, "detune")
    num("o%d_width" % o, "Osc %d width" % o, "lin", 0, 1, 0.5, "pct")
    num("o%d_level" % o, "Osc %d level" % o, "lin", 0, 1, level, "pct")

for f, (ftype, cut, env) in ((1, ("LP24", 1200, 0.25)), (2, ("Off", 8000, 0.0))):
    enum("f%d_type" % f, "Filter %d type" % f, FILTER_TYPES, ftype)
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


def norm(p):
    """The default as MPC's 0..1 value."""
    lo, hi, d = p["lo"], p["hi"], p["default"]
    if p["curve"] == "log":
        return math.log(d / lo) / math.log(hi / lo)
    return (d - lo) / (hi - lo) if hi > lo else 0.0


def params_json():
    out = []
    for p in P:
        if p["curve"] == "readout":
            out.append({"key": p["key"], "name": p["name"], "min": 0, "max": 0, "display": "string",
                        "type": "readout"})
        elif p["curve"] == "enum":
            out.append({"key": p["key"], "name": p["name"], "options": p["options"],
                        "default": p["options"][p["default"]]})
        else:
            out.append({"key": p["key"], "name": p["name"], "min": 0, "max": 1,
                        "default": round(norm(p), 6), "display": "string"})
    return {"name": VST["name"], "params": out}


# --- touchscreen pages ---------------------------------------------------------------------
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


def knob(cx, cy, label, key):
    return 'knob cx=%d cy=%d r=%d label="%s" key=%s' % (cx, cy, R, label, key)


def pages():
    L = [THEME]
    status = 'readout cx=640 cy=126 w=1212 h=40 key=status'

    # OSC: wave selector + 8 knobs per oscillator, knobs left-to-right = Q-Links 1-8 / 9-16
    osc_knobs = [("pos", "POSITION"), ("oct", "OCTAVE"), ("semi", "SEMI"), ("fine", "FINE"),
                 ("uni", "UNISON"), ("detune", "DETUNE"), ("width", "WIDTH"), ("level", "LEVEL")]
    L.append("[tab OSC]")
    L.append(status)
    q = []
    for o, top in zip((1, 2), ROW_Y):
        L.append('frame x=34 y=%d w=1212 h=270 title="OSCILLATOR %d"' % (top, o))
        L.append('enum_h cx=740 cy=%d sw=150 key=o%d_wave' % (top + ROW_ENUM, o))
        for cx, (k, lab) in zip(SLOT8, osc_knobs):
            L.append(knob(cx, top + ROW_KNOB, lab, "o%d_%s" % (o, k)))
            q.append("o%d_%s" % (o, k))
    L.append('qlinks "OSC" = ' + ",".join(q))

    # FILTER: type selector + 5 knobs per filter, routing next to the status line
    flt_knobs = [("cut", "CUTOFF"), ("res", "RESO"), ("env", "ENV 2"), ("key", "KEYTRACK"), ("drive", "DRIVE")]
    L.append("[tab FILTER]")
    L.append('readout cx=440 cy=126 w=812 h=40 key=status')
    L.append('enum_h cx=1080 cy=126 sw=150 key=routing')
    for f, top in zip((1, 2), ROW_Y):
        L.append('frame x=34 y=%d w=1212 h=270 title="FILTER %d"' % (top, f))
        L.append('enum_h cx=740 cy=%d sw=112 key=f%d_type' % (top + ROW_ENUM, f))
        for cx, (k, lab) in zip(SLOT8, flt_knobs):
            L.append(knob(cx, top + ROW_KNOB, lab, "f%d_%s" % (f, k)))
    L.append('qlinks "FILTER" = ' + ",".join(
        ["f1_cut", "f1_res", "f1_env", "f1_drive", "f2_cut", "f2_res", "f2_env", "f2_drive",
         "f1_key", "f2_key", "e2_a", "e2_d", "e2_s", "e2_r", "e2_pos", "volume"]))

    # ENV: amp + mod envelopes, output knobs on the right
    L.append("[tab ENV]")
    L.append(status)
    for e, top, title, last in ((1, ROW_Y[0], "ENV 1   (AMP)", ("vel", "VELOCITY")),
                                (2, ROW_Y[1], "ENV 2   (MOD)", ("pos", "> WAVE POS"))):
        L.append('frame x=34 y=%d w=890 h=270 title="%s"' % (top, title))
        for cx, (k, lab) in zip(SLOT8, [("a", "ATTACK"), ("d", "DECAY"), ("s", "SUSTAIN"), ("r", "RELEASE"), last]):
            L.append(knob(cx, top + ROW_KNOB, lab, "e%d_%s" % (e, k)))
    L.append('frame x=936 y=156 w=310 h=554 title="OUTPUT"')
    L.append(knob(1091, ROW_Y[0] + ROW_KNOB, "VOLUME", "volume"))
    L.append(knob(1091, ROW_Y[1] + ROW_KNOB, "VOICES", "voices"))
    L.append('qlinks "ENV" = ' + ",".join(
        ["e1_a", "e1_d", "e1_s", "e1_r", "e2_a", "e2_d", "e2_s", "e2_r",
         "e1_vel", "e2_pos", "volume", "voices", "o1_pos", "o2_pos", "f1_cut", "f2_cut"]))
    return "\n".join(L) + "\n"


# --- C++ header ----------------------------------------------------------------------------
CURVE = {"readout": "Readout", "enum": "Enum", "lin": "Lin", "log": "Log", "int": "Int"}
FMT = {"none": "None", "enum": "Enum", "pct": "Percent", "bipct": "Bipolar", "hz": "Hz", "time": "Time",
       "semi": "Semi", "cent": "Cent", "oct": "Oct", "count": "Count", "db": "Db", "detune": "Detune"}


def header():
    ids = ",\n".join("    P_%s%s" % (p["key"].upper(), " = 0" if i == 0 else "") for i, p in enumerate(P))
    specs = ",\n".join("    {Curve::%s, Fmt::%s, %sf, %sf}  /* %s */" % (
        CURVE[p["curve"]], FMT[p["fmt"]], float(p["lo"]), float(p["hi"]), p["key"]) for p in P)
    return """// generated by surface/surface.py: do not edit
#pragma once

namespace pf {

enum ParamId : int {
%s,
    P_COUNT
};

enum class Curve : unsigned char { Readout, Enum, Lin, Log, Int };
enum class Fmt : unsigned char { None, Enum, Percent, Bipolar, Hz, Time, Semi, Cent, Oct, Count, Db, Detune };

struct ParamSpec { Curve curve; Fmt fmt; float lo, hi; };

static const ParamSpec PARAM_SPECS[P_COUNT] = {
%s
};

constexpr int kNumWaves = %d;
constexpr int kNumFilterTypes = %d;
constexpr int kParamMaxVoices = %d;   // = kMaxVoices in dsp/synth.h
constexpr int kParamMaxUnison = %d;   // = kMaxUnison in dsp/synth.h

} // namespace pf
""" % (ids, specs, len(WAVES), len(FILTER_TYPES), MAX_VOICES, MAX_UNISON)


def main():
    os.makedirs(os.path.join(HERE, "build"), exist_ok=True)
    keys = [p["key"] for p in P]
    assert len(set(keys)) == len(keys), "duplicate parameter key"
    with open(os.path.join(HERE, "params.json"), "w", newline="\n") as f:
        json.dump(params_json(), f, indent=1)
    with open(os.path.join(HERE, "layout.conf"), "w", newline="\n") as f:
        f.write(pages())
    with open(os.path.join(HERE, "vst.json"), "w", newline="\n") as f:
        json.dump(VST, f, indent=1)
    with open(os.path.join(HERE, "build", "param_ids.h"), "w", newline="\n") as f:
        f.write(header())
    print("surface: %d parameters" % len(P))


if __name__ == "__main__":
    main()
