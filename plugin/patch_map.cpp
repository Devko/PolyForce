#include "patch_map.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pf {

static_assert(kParamMaxVoices == kMaxVoices && kParamMaxUnison == kMaxUnison,
              "surface.py MAX_VOICES/MAX_UNISON must match dsp/synth.h kMaxVoices/kMaxUnison");
// patchFromParams walks oscillator 2 / filter 2 / envelope 2 at a fixed offset from 1.
static_assert(P_O2_SUB_LEVEL - P_O2_WAVE == P_O1_SUB_LEVEL - P_O1_WAVE, "oscillator params out of order");
static_assert(P_O2_TABLE - P_O1_TABLE == P_O2_WAVE - P_O1_WAVE, "oscillator params out of order");
static_assert(P_F2_DRIVE - P_F2_TYPE == P_F1_DRIVE - P_F1_TYPE, "filter params out of order");
static_assert(kNumFilterTypes == kNumFilterModes, "surface.py FILTER_TYPES must match dsp FilterType");
static_assert(kNumLfoWaves == LW_COUNT && kNumSyncDivisions == kNumSyncDivs, "surface.py LFO lists must match dsp/mod.h");
static_assert(kNumModSources == MS_COUNT && kNumModTargets == MT_COUNT && kNumModModifiers == MM_COUNT &&
                  kNumModSlots == kModSlots, "surface.py matrix lists must match dsp/mod.h");
static_assert(P_L2_DEPTH - P_L2_WAVE == P_L1_DEPTH - P_L1_WAVE, "LFO params out of order");
static_assert(P_M12_A2 - P_M12_SRC == P_M1_A2 - P_M1_SRC && P_M12_SRC - P_M1_SRC == 11 * (P_M2_SRC - P_M1_SRC),
              "matrix params out of order");
static_assert(P_XY4_Y - P_XY1_X == 7, "XY params out of order");
static_assert(P_M1_A1 == P_M1_T1 + 1 && P_M1_A2 == P_M1_T2 + 1, "a matrix amount must follow its target (surface.cpp)");
static_assert(kNumArpDirs == AD_COUNT && kNumSeqSteps == kSeqSteps && kNumShapeSteps == kShapeSteps,
              "surface.py sequencer lists must match dsp/notegen.h");
static_assert(P_S16_NOTE - P_S1_NOTE == 15 && P_S16_VEL - P_S1_VEL == 15 && P_S16_MOD - P_S1_MOD == 15, "step params out of order");
static_assert(P_SH4_8 - P_SH4_MODE == 8 && P_SH2_MODE - P_SH1_MODE == 9, "shape params out of order");
static_assert(P_E2_R - P_E2_A == P_E1_R - P_E1_A, "envelope params out of order");

// Every member of a repeated block, not just its ends: block 2 (3, ...) must hold the same
// keys as block 1 in the same order ("o1_pan" ~ "o2_pan": the part after the first '_').
namespace {
constexpr const char* suffix(const char* k) {
    while (*k && *k != '_') ++k;
    return k;
}
constexpr bool same(const char* a, const char* b) {
    while (*a && *a == *b) ++a, ++b;
    return *a == *b;
}
constexpr bool sameBlock(int first, int other, int count) {
    for (int k = 0; k < count; ++k)
        if (!same(suffix(PARAM_INFO[first + k].key), suffix(PARAM_INFO[other + k].key))) return false;
    return true;
}
constexpr bool endsWith(const char* k, const char* tail) {
    int n = 0, m = 0;
    while (k[n]) ++n;
    while (tail[m]) ++m;
    return n >= m && same(k + n - m, tail);
}
constexpr bool matrixInOrder() {
    for (int s = 1; s < kModSlots; ++s)
        if (!sameBlock(P_M1_SRC, P_M1_SRC + s * (P_M2_SRC - P_M1_SRC), P_M2_SRC - P_M1_SRC)) return false;
    return true;
}
constexpr bool runOf(int first, int count, const char* tail) {   // first..first+count-1 all end in tail
    for (int k = 0; k < count; ++k)
        if (!endsWith(PARAM_INFO[first + k].key, tail)) return false;
    return true;
}
constexpr bool xyInOrder() {   // xy1_x, xy1_y, xy2_x, ...
    for (int i = 0; i < kXyAxes; ++i)
        if (PARAM_INFO[P_XY1_X + i].key[2] != '1' + i / 2 || !endsWith(PARAM_INFO[P_XY1_X + i].key, i % 2 ? "_y" : "_x")) return false;
    return true;
}
constexpr bool shapesInOrder() {   // shN_mode, shN_1 .. shN_8
    for (int l = 0; l < kShapeLanes; ++l) {
        const int base = P_SH1_MODE + l * (P_SH2_MODE - P_SH1_MODE);
        if (!endsWith(PARAM_INFO[base].key, "_mode") || PARAM_INFO[base].key[2] != '1' + l) return false;
        for (int k = 0; k < kShapeSteps; ++k) {
            const char* key = PARAM_INFO[base + 1 + k].key;
            if (key[2] != '1' + l || key[4] != '1' + k || key[5] != 0) return false;
        }
    }
    return true;
}
} // namespace
static_assert(sameBlock(P_O1_WAVE, P_O2_WAVE, P_O1_SUB_LEVEL - P_O1_WAVE + 1), "oscillator 2 params must mirror oscillator 1");
static_assert(sameBlock(P_F1_TYPE, P_F2_TYPE, P_F1_DRIVE - P_F1_TYPE + 1), "filter 2 params must mirror filter 1");
static_assert(sameBlock(P_L1_WAVE, P_L2_WAVE, P_L1_DEPTH - P_L1_WAVE + 1), "LFO 2 params must mirror LFO 1");
static_assert(sameBlock(P_E1_A, P_E2_A, 4), "envelope 2 stages must mirror envelope 1");
static_assert(matrixInOrder(), "every matrix slot must mirror slot 1");
static_assert(xyInOrder(), "XY params must run xy1_x, xy1_y, xy2_x ...");
static_assert(shapesInOrder(), "shape params must run shN_mode, shN_1 .. shN_8");
static_assert(runOf(P_S1_NOTE, kSeqSteps, "_note") && runOf(P_S1_VEL, kSeqSteps, "_vel") && runOf(P_S1_MOD, kSeqSteps, "_mod"),
              "step params out of order");
static_assert(PARAM_SPECS[P_M1_A1].fmt == Fmt::ModAmt && PARAM_SPECS[P_M12_A2].fmt == Fmt::ModAmt,
              "a matrix amount must follow its target (surface.cpp)");
// Option lists in surface.py against the engine's enums.
static_assert(PARAM_INFO[P_O1_WAVE].nopts == OW_NOISE + 1 && PARAM_INFO[P_O1_ROUTE].nopts == RT_DIRECT + 1 &&
                  PARAM_INFO[P_NOISE_ROUTE].nopts == RT_DIRECT + 1 && PARAM_INFO[P_O1_SUB_WAVE].nopts == CW_SQUARE + 1 &&
                  PARAM_INFO[P_O1_PHMODE].nopts == PH_FREE + 1,
              "oscillator option lists must match dsp/synth.h");
static_assert(PARAM_INFO[P_VMODE].nopts == VM_LEGATO + 1 && PARAM_INFO[P_STEAL].nopts == ST_KEEP_HIGH + 1 &&
                  PARAM_INFO[P_GLIDE_MODE].nopts == GL_LEGATO + 1 && PARAM_INFO[P_ENGINE].nopts == EN_DIRTY + 1,
              "voice / engine option lists must match dsp/synth.h");
static_assert(PARAM_INFO[P_L1_TRIG].nopts == LT_GLOBAL + 1 && PARAM_INFO[P_SEQ_MODE].nopts == SQ_SEQ + 1 &&
                  PARAM_INFO[P_SH1_MODE].nopts == SM_SMOOTH + 1,
              "LFO / sequencer option lists must match the engine");

float paramValue(int id, float n) {
    if (id < 0 || id >= P_COUNT) return 0.0f;
    const ParamSpec& s = PARAM_SPECS[id];
    n = std::clamp(n, 0.0f, 1.0f);
    switch (s.curve) {
        case Curve::Lin:  return s.lo + n * (s.hi - s.lo);
        case Curve::Log:  return s.lo * std::pow(s.hi / s.lo, n);
        case Curve::Int:  return std::round(s.lo + n * (s.hi - s.lo));
        case Curve::Enum: return std::round(n * s.hi);   // lo = 0, hi = options - 1
        case Curve::Pow:  return s.hi * n * n * n;       // 0..hi, fine near 0 (times that may be 0)
        default:          return 0.0f;
    }
}

float paramNorm(int id, float v) {
    if (id < 0 || id >= P_COUNT) return 0.0f;
    const ParamSpec& s = PARAM_SPECS[id];
    float n = 0.0f;
    switch (s.curve) {
        case Curve::Log: n = v > 0.0f ? std::log(v / s.lo) / std::log(s.hi / s.lo) : 0.0f; break;
        case Curve::Pow: n = v > 0.0f && s.hi > 0.0f ? std::cbrt(v / s.hi) : 0.0f; break;
        case Curve::Lin:
        case Curve::Int:
        case Curve::Enum: n = s.hi > s.lo ? (v - s.lo) / (s.hi - s.lo) : 0.0f; break;
        default: break;
    }
    return std::isfinite(n) ? std::clamp(n, 0.0f, 1.0f) : 0.0f;   // values come from saved state text too
}

std::string paramDisplay(int id, float n) {
    if (id < 0 || id >= P_COUNT) return {};
    const float v = paramValue(id, n);
    char b[32];
    switch (PARAM_SPECS[id].fmt) {
        case Fmt::Enum: {
            const int i = static_cast<int>(v);
            return i >= 0 && i < PARAM_INFO[id].nopts ? PARAM_INFO[id].opts[i] : "";
        }
        case Fmt::Percent: std::snprintf(b, sizeof b, "%.0f%%", v * 100.0f); break;
        case Fmt::Bipolar:
            std::snprintf(b, sizeof b, std::fabs(v) < 0.005f ? "0%%" : "%+.0f%%", v * 100.0f);
            break;
        // Unit changes where the rounded text would reach the next unit ("1000 Hz" is "1.00 kHz").
        case Fmt::Hz:
            if (v < 999.5f) std::snprintf(b, sizeof b, "%.0f Hz", v);
            else std::snprintf(b, sizeof b, v < 9995.0f ? "%.2f kHz" : "%.1f kHz", v / 1000.0f);
            break;
        case Fmt::Time:
            if (v < 0.00995f) std::snprintf(b, sizeof b, "%.1f ms", v * 1000.0f);
            else if (v < 0.9995f) std::snprintf(b, sizeof b, "%.0f ms", v * 1000.0f);
            else std::snprintf(b, sizeof b, "%.2f s", v);
            break;
        case Fmt::Semi: std::snprintf(b, sizeof b, v == 0.0f ? "0 st" : "%+.0f st", v); break;
        case Fmt::Cent: {
            const long c = std::lround(v);
            if (c == 0) return "0 ct";
            std::snprintf(b, sizeof b, "%+ld ct", c);
            break;
        }
        case Fmt::Oct: std::snprintf(b, sizeof b, v == 0.0f ? "0 oct" : "%+.0f oct", v); break;
        case Fmt::Count: std::snprintf(b, sizeof b, "%.0f", v); break;
        case Fmt::Db:
            if (v <= -59.5f) return "-inf dB";
            std::snprintf(b, sizeof b, "%.1f dB", std::fabs(v) < 0.05f ? 0.0f : v);   // never "-0.0 dB"
            break;
        case Fmt::Detune: std::snprintf(b, sizeof b, "%.1f ct", 100.0f * v * v); break;
        case Fmt::Pan:
            if (std::lround(std::fabs(v) * 100.0f) == 0) return "C";
            std::snprintf(b, sizeof b, "%c%ld", v < 0.0f ? 'L' : 'R', std::lround(std::fabs(v) * 100.0f));
            break;
        case Fmt::Degrees: std::snprintf(b, sizeof b, "%.0f deg", v * 360.0f); break;
        case Fmt::LfoHz: std::snprintf(b, sizeof b, v < 0.995f ? "%.2f Hz" : (v < 9.95f ? "%.1f Hz" : "%.0f Hz"), v); break;
        case Fmt::ModAmt: std::snprintf(b, sizeof b, std::fabs(v) < 0.005f ? "0%%" : "%+.0f%%", v * 100.0f); break;
        default: return {};
    }
    return b;
}

Patch patchFromParams(const float* norm) {
    auto V = [norm](int id) { return paramValue(id, norm[id]); };
    Patch p;
    p.volumeDb = V(P_VOLUME);
    p.voices = static_cast<int>(V(P_VOICES));
    p.parallel = V(P_ROUTING) > 0.5f;
    for (int o = 0; o < 2; ++o) {
        const int d = o * (P_O2_WAVE - P_O1_WAVE);
        OscPatch& x = p.osc[o];
        x.pos = V(P_O1_POS + d);
        x.pitch = 12.0f * V(P_O1_OCT + d) + V(P_O1_SEMI + d) + V(P_O1_FINE + d) / 100.0f;
        x.unison = static_cast<int>(V(P_O1_UNI + d));
        x.detune = V(P_O1_DETUNE + d);
        x.width = V(P_O1_WIDTH + d);
        x.level = V(P_O1_LEVEL + d);
        x.wave = static_cast<int>(V(P_O1_WAVE + d));
        x.pan = V(P_O1_PAN + d);
        x.phase = V(P_O1_PHASE + d);
        x.phaseMode = static_cast<int>(V(P_O1_PHMODE + d));
        x.route = static_cast<int>(V(P_O1_ROUTE + d));
        x.subWave = static_cast<int>(V(P_O1_SUB_WAVE + d));
        x.subTune = V(P_O1_SUB_TUNE + d);
        x.subLevel = V(P_O1_SUB_LEVEL + d);
    }
    p.noise.level = V(P_NOISE_LEVEL);
    p.noise.color = V(P_NOISE_COLOR);
    p.noise.route = static_cast<int>(V(P_NOISE_ROUTE));
    for (int f = 0; f < 2; ++f) {
        const int d = f * (P_F2_TYPE - P_F1_TYPE);
        FilterPatch& x = p.flt[f];
        x.type = static_cast<int>(V(P_F1_TYPE + d));
        x.cutoffHz = V(P_F1_CUT + d);
        x.res = V(P_F1_RES + d);
        x.env = V(P_F1_ENV + d);
        x.key = V(P_F1_KEY + d);
        x.drive = V(P_F1_DRIVE + d);
    }
    for (int e = 0; e < 2; ++e) {
        const int d = e * (P_E2_A - P_E1_A);
        p.env[e].a = V(P_E1_A + d);
        p.env[e].d = V(P_E1_D + d);
        p.env[e].s = V(P_E1_S + d);
        p.env[e].r = V(P_E1_R + d);
    }
    p.voiceMode = static_cast<int>(V(P_VMODE));
    p.steal = static_cast<int>(V(P_STEAL));
    p.sameNoteNew = V(P_SAME_NOTE) > 0.5f;
    p.glideMode = static_cast<int>(V(P_GLIDE_MODE));
    p.glideRate = V(P_GLIDE_TYPE) > 0.5f;
    p.glideTime = V(P_GLIDE);
    p.bendUp = V(P_BEND_UP);
    p.bendDown = V(P_BEND_DN);
    p.velCurve = V(P_VEL_CURVE);
    p.engine = static_cast<int>(V(P_ENGINE));
    p.velSens = V(P_E1_VEL);
    p.env2Pos = V(P_E2_POS);
    p.env[1].vel = V(P_E2_VEL);
    p.env[1].loop = V(P_E2_LOOP) > 0.5f;
    for (int l = 0; l < 2; ++l) {
        const int d = l * (P_L2_WAVE - P_L1_WAVE);
        LfoPatch& x = p.lfo[l];
        x.wave = static_cast<int>(V(P_L1_WAVE + d));
        x.sync = V(P_L1_SYNC + d) > 0.5f;
        x.rateHz = V(P_L1_RATE + d);
        x.div = static_cast<int>(V(P_L1_DIV + d));
        x.phase = V(P_L1_PHASE + d);
        x.delay = V(P_L1_DELAY + d);
        x.fade = V(P_L1_FADE + d);
        x.trig = static_cast<int>(V(P_L1_TRIG + d));
        x.unipolar = V(P_L1_POLAR + d) > 0.5f;
        x.depth = V(P_L1_DEPTH + d);
    }
    for (int k = 0; k < kModSlots; ++k) {
        const int d = k * (P_M2_SRC - P_M1_SRC);
        ModSlot& s = p.mod[k];
        s.src = static_cast<int>(V(P_M1_SRC + d));
        s.via = static_cast<int>(V(P_M1_VIA + d));
        s.mod = static_cast<int>(V(P_M1_MOD + d));
        s.modAmt = V(P_M1_MODAMT + d);
        s.tgt[0] = static_cast<int>(V(P_M1_T1 + d));
        s.amt[0] = V(P_M1_A1 + d);
        s.tgt[1] = static_cast<int>(V(P_M1_T2 + d));
        s.amt[1] = V(P_M1_A2 + d);
    }
    for (int x = 0; x < 4; ++x) {
        p.xy[2 * x] = V(P_XY1_X + 2 * x);
        p.xy[2 * x + 1] = V(P_XY1_Y + 2 * x);
    }
    return p;
}

SeqPatch seqFromParams(const float* norm) {
    auto V = [norm](int id) { return paramValue(id, norm[id]); };
    SeqPatch s;
    s.mode = static_cast<int>(V(P_SEQ_MODE));
    s.dir = static_cast<int>(V(P_ARP_DIR));
    s.octaves = static_cast<int>(V(P_ARP_OCT));
    s.rate = static_cast<int>(V(P_CLK_RATE));
    s.gate = V(P_CLK_GATE);
    s.swing = V(P_CLK_SWING);
    s.latch = V(P_ARP_LATCH) > 0.5f;
    s.pattern = V(P_ARP_PATTERN) > 0.5f;
    s.steps = static_cast<int>(V(P_SEQ_STEPS));
    s.record = V(P_SEQ_REC) > 0.5f;
    for (int k = 0; k < kSeqSteps; ++k) {
        s.note[k] = static_cast<int>(V(P_S1_NOTE + k));
        s.vel[k] = static_cast<int>(V(P_S1_VEL + k));
        s.mod[k] = V(P_S1_MOD + k);
    }
    s.shapeRate = static_cast<int>(V(P_SH_RATE));
    s.shapeSteps = static_cast<int>(V(P_SH_STEPS));
    for (int l = 0; l < kShapeLanes; ++l) {
        const int d = l * (P_SH2_MODE - P_SH1_MODE);
        s.shapeMode[l] = static_cast<int>(V(P_SH1_MODE + d));
        for (int k = 0; k < kShapeSteps; ++k) s.shape[l][k] = V(P_SH1_1 + d + k);
    }
    return s;
}

} // namespace pf
