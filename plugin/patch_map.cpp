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
static_assert(P_E2_R - P_E2_A == P_E1_R - P_E1_A, "envelope params out of order");

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
        case Fmt::Hz:
            if (v < 1000.0f) std::snprintf(b, sizeof b, "%.0f Hz", v);
            else std::snprintf(b, sizeof b, v < 10000.0f ? "%.2f kHz" : "%.1f kHz", v / 1000.0f);
            break;
        case Fmt::Time:
            if (v < 0.01f) std::snprintf(b, sizeof b, "%.1f ms", v * 1000.0f);
            else if (v < 1.0f) std::snprintf(b, sizeof b, "%.0f ms", v * 1000.0f);
            else std::snprintf(b, sizeof b, "%.2f s", v);
            break;
        case Fmt::Semi: std::snprintf(b, sizeof b, v == 0.0f ? "0 st" : "%+.0f st", v); break;
        case Fmt::Cent: std::snprintf(b, sizeof b, std::fabs(v) < 0.5f ? "0 ct" : "%+.0f ct", v); break;
        case Fmt::Oct: std::snprintf(b, sizeof b, v == 0.0f ? "0 oct" : "%+.0f oct", v); break;
        case Fmt::Count: std::snprintf(b, sizeof b, "%.0f", v); break;
        case Fmt::Db:
            if (v <= -59.5f) return "-inf dB";
            std::snprintf(b, sizeof b, "%.1f dB", v);
            break;
        case Fmt::Detune: std::snprintf(b, sizeof b, "%.1f ct", 100.0f * v * v); break;
        case Fmt::Pan:
            if (std::fabs(v) < 0.005f) return "C";
            std::snprintf(b, sizeof b, "%c%.0f", v < 0.0f ? 'L' : 'R', std::fabs(v) * 100.0f);
            break;
        case Fmt::Degrees: std::snprintf(b, sizeof b, "%.0f deg", v * 360.0f); break;
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
    p.velSens = V(P_E1_VEL);
    p.env2Pos = V(P_E2_POS);
    return p;
}

} // namespace pf
