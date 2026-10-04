#include "synth.h"

#include "simd.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace pf {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kHeadroom = 0.25f;   // -12 dB per voice, so a 4-note chord at full level doesn't clip
constexpr float kEnvOctaves = 8.0f;  // filter env amount +-1 = +-8 octaves

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

// 2^x, relative error < 1.1e-7 (a degree-6 polynomial on the fraction, the exponent by bits):
// libm's exp2f costs ~10x as much, and the audio thread calls this per voice per chunk.
inline float exp2Fast(float x) {
    x = clampf(x, -126.0f, 126.0f);
    const int i = static_cast<int>(x < 0.0f ? x - 1.0f : x);   // floor, or one below: f stays in [0, 1]
    const float f = x - static_cast<float>(i);
    float p = 2.120030258e-4f;
    p = p * f + 1.258059288e-3f;
    p = p * f + 9.664113633e-3f;
    p = p * f + 5.549038202e-2f;
    p = p * f + 2.402283251e-1f;
    p = p * f + 6.931471229e-1f;
    p = p * f + 1.0f;
    const uint32_t bits = static_cast<uint32_t>(i + 127) << 23;
    float scale;
    std::memcpy(&scale, &bits, sizeof scale);
    return p * scale;
}

// tan(w) for 0 <= w < pi/2 (filter coefficients), relative error < 3e-7: an odd polynomial on
// [0, pi/4], and tan(w) = 1 / tan(pi/2 - w) above.
inline float tanFast(float w) {
    constexpr float kQuarter = 0.785398163f, kHalf = 1.570796327f;
    const bool upper = w > kQuarter;
    const float x = upper ? kHalf - w : w, t = x * x;
    float p = 8.657055907e-3f;
    p = p * t + 4.348253831e-3f;
    p = p * t + 2.366562374e-2f;
    p = p * t + 5.362507701e-2f;
    p = p * t + 1.333622932e-1f;
    p = p * t + 3.333325386e-1f;
    p = p * t + 1.0f;
    const float r = p * x;
    return upper ? 1.0f / std::max(r, 1e-12f) : r;
}

// Equal-power pan gains for a balance -1..1 (sqrt 2 at the sides, 1 in the middle): sin/cos
// as short series on [0, pi/2] (error < 4e-6).
inline void panGains(float pan, float& gl, float& gr) {
    const float a = (clampf(pan, -1.0f, 1.0f) + 1.0f) * 0.785398163f;   // 0 .. pi/2
    auto sinq = [](float x) {
        const float x2 = x * x;
        return x * (1.0f + x2 * (-1.666666667e-1f + x2 * (8.333333333e-3f + x2 * (-1.984126984e-4f + x2 * 2.755731922e-6f))));
    };
    gl = sinq(1.570796327f - a) * 1.41421356f;
    gr = sinq(a) * 1.41421356f;
}

inline float noteHz(float note) { return 440.0f * exp2Fast((note - 69.0f) * (1.0f / 12.0f)); }
inline float hzNote(float hz) { return 69.0f + 12.0f * std::log2(std::max(hz, 1.0f) / 440.0f); }

// 2^x for |x| <= ~0.1 (detune ratios), error < 1e-6.
inline float exp2Small(float x) { return 1.0f + x * (0.69314718f + x * (0.24022651f + x * 0.05550411f)); }

// tanh-like saturator, exactly +-1 from |x| = 3 on.
inline float softclip(float x) {
    x = clampf(x, -3.0f, 3.0f);
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

// Per-sample one-pole step for a time constant of `tau` seconds.
inline float onePole(float tau, float sr) { return 1.0f - std::exp(-1.0f / (std::max(tau, 1e-5f) * sr)); }

// A one-pole step `k` meant for a whole chunk, for a chunk of `n` samples (render() splits
// chunks at MIDI events and sequencer steps: a short chunk must not smooth a full step).
inline float chunkStep(float k, int n) {
    return n >= kChunk ? k : 1.0f - std::pow(1.0f - k, static_cast<float>(n) / static_cast<float>(kChunk));
}

// The envelope over a run of samples: one-pole segments, attack aiming at 1.2 and stopping at
// 1.0, decay settling on the sustain level (looping: back to attack near it), release to -80 dB.
// Each stage has its own tight loop (no per-sample switch). Store: out[i] = the value after
// sample i (the amp envelope); else only the end state matters (the mod envelope is read once
// per chunk).
template <bool Store>
inline void envRun(Env& e, const EnvCoef& c, bool loop, float* out, int n) {
    if (!Store && n == kChunk) {   // a whole chunk without a stage change: one step, closed form
        float w = e.v;
        switch (e.stage) {
            case Attack: w = 1.2f - (1.2f - e.v) * c.attN; break;   // reaching 1.0 inside: sample by sample below
            case Decay: w = c.sus + (e.v - c.sus) * c.decN; break;
            case Release: w = e.v * c.relN; break;
            case Idle: return;
        }
        const bool same = e.stage == Attack ? w < 1.0f
                        : e.stage == Release ? w >= 1e-4f
                        : (!loop || (e.v - c.sus >= 0.01f && w - c.sus >= 0.01f));   // decay moves monotonically to sus
        if (same) {
            e.v = w;
            return;
        }
    }
    int i = 0;
    float v = e.v;
    while (i < n) {
        switch (e.stage) {
            case Attack:
                for (; i < n; ++i) {
                    v += (1.2f - v) * c.att;
                    if (v >= 1.0f) {
                        v = 1.0f;
                        e.stage = Decay;
                        if (Store) out[i] = v;
                        ++i;
                        break;
                    }
                    if (Store) out[i] = v;
                }
                break;
            case Decay:
                if (loop) {
                    for (; i < n; ++i) {
                        v += (c.sus - v) * c.dec;
                        if (Store) out[i] = v;
                        if (v - c.sus < 0.01f) {
                            e.stage = Attack;
                            ++i;
                            break;
                        }
                    }
                } else {
                    for (; i < n; ++i) {
                        v += (c.sus - v) * c.dec;
                        if (Store) out[i] = v;
                    }
                }
                break;
            case Release:
                for (; i < n; ++i) {
                    v -= v * c.rel;
                    if (v < 1e-4f) {
                        v = 0.0f;
                        e.stage = Idle;
                        if (Store) out[i] = v;
                        ++i;
                        break;
                    }
                    if (Store) out[i] = v;
                }
                break;
            case Idle:
                if (Store)
                    for (; i < n; ++i) out[i] = v;
                i = n;
                break;
        }
    }
    e.v = v;
}

// Zeroes a chunk buffer with vector stores (a memset call per small buffer costs more than
// the work it clears).
inline void zeroChunk(float* p) {
    typedef float f4 __attribute__((vector_size(16)));
    const f4 z = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int i = 0; i < kChunk; i += 4) std::memcpy(p + i, &z, sizeof z);
}

inline SvfCoef makeSvf(float g, float k) {
    SvfCoef c;
    c.g = g;
    c.k = k;
    c.a1 = 1.0f / (1.0f + g * (g + k));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
    return c;
}

// One SVF step: v1 = band, v2 = low; high = v0 - k*v1 - v2.
inline void svf(Svf& s, const SvfCoef& c, float v0, float& v1, float& v2) {
    const float v3 = v0 - s.ic2;
    v1 = c.a1 * s.ic1 + c.a2 * v3;
    v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
    s.ic1 = 2.0f * v1 - s.ic1;
    s.ic2 = 2.0f * v2 - s.ic2;
}

// One voice alone (a quad with one live lane costs more than this): the same filters, scalar.
template <bool Dirty> inline void stepS(Svf& s, const SvfCoef& c, float v0, float& v1, float& v2) {
    const float v3 = v0 - s.ic2;
    v1 = c.a1 * s.ic1 + c.a2 * v3;
    v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
    s.ic1 = Dirty ? softclip(2.0f * v1 - s.ic1) : 2.0f * v1 - s.ic1;
    s.ic2 = 2.0f * v2 - s.ic2;
}

template <bool Dirty>
void svfScalar(int type, Svf& s1, Svf& s2, const SvfCoef& c, const SvfCoef& fl, float* x, int n) {
    float b, l, b2, l2;
    switch (type) {
        case F_LP12: for (int i = 0; i < n; ++i) { stepS<Dirty>(s1, c, x[i], b, l); x[i] = l; } break;
        case F_LP24: for (int i = 0; i < n; ++i) { stepS<Dirty>(s1, c, x[i], b, l); stepS<false>(s2, fl, l, b2, l2); x[i] = l2; } break;
        case F_HP12: for (int i = 0; i < n; ++i) { stepS<Dirty>(s1, c, x[i], b, l); x[i] = x[i] - c.k * b - l; } break;
        case F_HP24:
            for (int i = 0; i < n; ++i) {
                stepS<Dirty>(s1, c, x[i], b, l);
                const float h = x[i] - c.k * b - l;
                stepS<false>(s2, fl, h, b2, l2);
                x[i] = h - fl.k * b2 - l2;
            }
            break;
        case F_BP: for (int i = 0; i < n; ++i) { stepS<Dirty>(s1, c, x[i], b, l); x[i] = c.k * b; } break;
        case F_NOTCH: for (int i = 0; i < n; ++i) { stepS<Dirty>(s1, c, x[i], b, l); x[i] -= c.k * b; } break;
        default: for (int i = 0; i < n; ++i) { stepS<Dirty>(s1, c, x[i], b, l); x[i] = 2.0f * l - x[i] + c.k * b; } break;
    }
}

// The state-variable filters four voices at a time (one voice per lane, dsp/simd.h): the same
// step as svf(), on vectors. Dirty saturates the band (resonant) state inside the loop, so
// loud resonance compresses and growls instead of ringing clean.
struct SvfV { f4 k, a1, a2, a3; };
struct DriveV { f4 pre, post, wet; };

template <bool Dirty> inline void stepV(f4& ic1, f4& ic2, const SvfV& c, f4 v0, f4& v1, f4& v2) {
    const f4 v3 = v0 - ic2;
    v1 = c.a1 * ic1 + c.a2 * v3;
    v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
    ic1 = Dirty ? softclip4(splat(2.0f) * v1 - ic1) : splat(2.0f) * v1 - ic1;
    ic2 = splat(2.0f) * v2 - ic2;
}

// One sample of a filter type; st = stage 1 (ic1, ic2), stage 2 (ic1, ic2). The 24 dB types
// add a second stage without extra resonance (fl).
template <int Type, bool Dirty> inline f4 svfV(f4 x, const SvfV& c, const SvfV& fl, f4* st) {
    f4 b, l;
    stepV<Dirty>(st[0], st[1], c, x, b, l);
    switch (Type) {
        case F_LP12: return l;
        case F_LP24: {
            f4 b2, l2;
            stepV<false>(st[2], st[3], fl, l, b2, l2);
            return l2;
        }
        case F_HP12: return x - c.k * b - l;
        case F_HP24: {
            const f4 h = x - c.k * b - l;
            f4 b2, l2;
            stepV<false>(st[2], st[3], fl, h, b2, l2);
            return h - fl.k * b2 - l2;
        }
        case F_BP: return c.k * b;           // scaled by k: unity gain at the centre whatever the resonance
        case F_NOTCH: return x - c.k * b;
        default: return splat(2.0f) * l - x + c.k * b;   // Peak: low - high
    }
}

// Four lanes over one chunk of one channel. bus: lane 0 of the quad at sample 0; one sample
// further is kMaxVoices floats on. One channel at a time keeps the recursion's state and
// coefficients in NEON's 16 registers (two interleaved channels spill).
template <int Type, bool Dirty>
void svfQuad(float* bus, int n, const SvfV& cIn, const SvfV& flIn, f4 (&stIn)[4]) {
    const SvfV c = cIn, fl = flIn;   // copies: the bus stores can't alias them, so they stay in registers
    f4 st[4] = {stIn[0], stIn[1], stIn[2], stIn[3]};
    for (int i = 0; i < n; ++i) {
        float* p = bus + i * kMaxVoices;
        store4(p, svfV<Type, Dirty>(load4(p), c, fl, st));
    }
    for (int j = 0; j < 4; ++j) stIn[j] = st[j];
}

// The drive stage before a filter, four lanes: memoryless, so it runs as its own pass.
inline void driveQuad(float* bus, int n, const DriveV& d) {
    for (int i = 0; i < n; ++i) {   // fades in with the drive amount (wet); a full-scale input stays ~unity
        float* p = bus + i * kMaxVoices;
        const f4 x = load4(p);
        store4(p, x + d.wet * (softclip4(x * d.pre) * d.post - x));
    }
}

using QuadFn = void (*)(float*, int, const SvfV&, const SvfV&, f4 (&)[4]);

template <int Type> QuadFn quadOf(bool dirty) { return dirty ? svfQuad<Type, true> : svfQuad<Type, false>; }

QuadFn quadFor(int type, bool dirty) {
    switch (type) {
        case F_LP12: return quadOf<F_LP12>(dirty);
        case F_LP24: return quadOf<F_LP24>(dirty);
        case F_HP12: return quadOf<F_HP12>(dirty);
        case F_HP24: return quadOf<F_HP24>(dirty);
        case F_BP: return quadOf<F_BP>(dirty);
        case F_NOTCH: return quadOf<F_NOTCH>(dirty);
        default: return quadOf<F_PEAK>(dirty);
    }
}

} // namespace

Synth::Synth(float sampleRate) : sr_(sampleRate) {
    builtinTables();   // build the shared tables now (UI thread), never on the audio thread
    classicTable(0);
    combMem_.assign(static_cast<size_t>(kMaxVoices) * 2 * 2 * kCombLen, 0.0f);
    for (int i = 0; i < kMaxVoices; ++i) {
        voices_[i].comb = &combMem_[static_cast<size_t>(i) * 2 * 2 * kCombLen];
        voices_[i].rng = 0x9e3779b9u ^ (0x85ebca6bu * static_cast<uint32_t>(i + 1));
    }
    setPatch(Patch{});
    fresh_ = true;     // the first real patch snaps its smoothers (no sweep from the defaults)
}

void Synth::setPatch(const Patch& p) {
    patch_ = p;
    patch_.voices = std::clamp(p.voices, 1, kMaxVoices);
    for (int o = 0; o < 2; ++o) updateOsc(o, patch_.osc[o]);
    for (int e = 0; e < 2; ++e) {   // knobs moved: every voice's own (modulated) copy is stale
        const EnvCoef c = envCoef(patch_.env[e], sr_);
        if (std::memcmp(&c, &envc_[e], sizeof c) != 0)
            for (auto& v : voices_) v.envc[e].dec = 0.0f;
        envc_[e] = c;
    }

    // The matrix: which slots do something, and their amounts in target units.
    nSlots_ = 0;
    envTargeted_ = false;
    for (int s = 0; s < kModSlots; ++s) {
        const ModSlot& ms = patch_.mod[s];
        bool live = false;
        for (int k = 0; k < 2; ++k) {
            const float a = clampf(ms.amt[k], -1.0f, 1.0f);
            float scale = a;
            switch (targetUnit(ms.tgt[k])) {
                case Unit::Semis: scale = a * std::fabs(a) * kPitchRange; break;
                case Unit::Cutoff: scale = a * kCutoffRange; break;
                case Unit::EnvTime: scale = a * kEnvOctavesMod; break;
                case Unit::LfoRate: scale = a * kLfoOctavesMod; break;
                case Unit::Color: scale = 2.0f * a; break;
                case Unit::None: scale = 0.0f; break;
                default: break;
            }
            slotScale_[s][k] = scale;
            live = live || (scale != 0.0f && ms.src != MS_NONE);
            const Unit u = targetUnit(ms.tgt[k]);
            if (scale != 0.0f && (u == Unit::EnvTime || u == Unit::EnvLevel)) envTargeted_ = true;
        }
        const float m = std::fabs(clampf(ms.modAmt, -1.0f, 1.0f));
        slotSlewK_[s] = onePole(0.001f * std::exp2(m * 11.0f), sr_ / static_cast<float>(kChunk));
        slotPeriod_[s] = sr_ / (0.5f * std::exp2(m * 7.0f));
        if (live) slots_[nSlots_++] = s;
    }
    for (int l = 0; l < 2; ++l) {
        lfoUsed_[l] = false;
        for (int k = 0; k < nSlots_; ++k) {
            const ModSlot& ms = patch_.mod[slots_[k]];
            lfoUsed_[l] = lfoUsed_[l] || ms.src == MS_LFO1 + l || ms.via == MS_LFO1 + l;
        }
    }
    volTarget_ = patch_.volumeDb <= -59.5f ? 0.0f : std::pow(10.0f, patch_.volumeDb / 20.0f) * kHeadroom;

    // Polyphony lowered while playing: let the voices above the new limit ring out.
    for (int i = voiceLimit(); i < kMaxVoices; ++i)
        if (voices_[i].active && (voices_[i].gate || voices_[i].sustained)) release(voices_[i]);

    if (fresh_) {   // first patch: start the smoothers on target instead of gliding from 0
        for (int f = 0; f < 2; ++f) cutSemi_[f] = hzNote(patch_.flt[f].cutoffHz);
        vol_ = volTarget_;
        fresh_ = false;
    }
}

void Synth::updateOsc(int o, const OscPatch& p) {
    OscState& s = osc_[o];
    switch (p.wave) {
        case OW_SINE: s.table = &classicTable(CW_SINE); break;
        case OW_TRIANGLE: s.table = &classicTable(CW_TRIANGLE); break;
        case OW_SAW: s.table = &classicTable(CW_SAW); break;
        case OW_SQUARE: s.table = &classicTable(CW_SQUARE); break;
        case OW_PULSE: s.table = &classicTable(CW_PULSE); break;
        case OW_NOISE: s.table = nullptr; break;
        default: s.table = p.table && p.table->frames > 0 ? p.table : &builtinTables()[0]; break;
    }

    const int n = std::clamp(p.unison, 1, kMaxUnison);
    if (n == s.keyUnison && p.detune == s.keyDetune && p.width == s.keyWidth && p.pan == s.keyPan) return;
    s.keyUnison = n;
    s.keyDetune = p.detune;
    s.keyWidth = p.width;
    s.keyPan = p.pan;

    // Unison voice u sits at d in [-1, 1]: detuned by d * spread and panned by d * width,
    // lowest voice left, highest right, the whole stack shifted by the pan. 1/sqrt(n) keeps
    // the loudness roughly constant.
    const float cents = 100.0f * p.detune * p.detune;
    const float gain = 1.0f / std::sqrt(static_cast<float>(n));   // the level is applied per voice (modulated)
    for (int u = 0; u < n; ++u) {
        const float d = n > 1 ? 2.0f * static_cast<float>(u) / static_cast<float>(n - 1) - 1.0f : 0.0f;
        s.spread[u] = d;
        s.ratio[u] = std::exp2(d * cents / 1200.0f);
        const float place = clampf(clampf(p.width, 0.0f, 1.0f) * d + clampf(p.pan, -1.0f, 1.0f), -1.0f, 1.0f);
        const float angle = (place + 1.0f) * kPi * 0.25f;   // equal-power pan
        s.gl[u] = std::cos(angle) * 1.41421356f * gain;
        s.gr[u] = std::sin(angle) * 1.41421356f * gain;
    }
    panGains(p.pan, s.subGl, s.subGr);
    s.n = n;
    s.cents = cents;
    s.maxRatio = n > 1 ? std::exp2(cents / 1200.0f) : 1.0f;
}

EnvCoef Synth::envCoef(const EnvPatch& e, float sr) {
    EnvCoef c;
    c.att = onePole(e.a / 1.7918f, sr);   // ln(1.2 / 0.2): aiming at 1.2, the curve crosses 1.0 at `a`
    c.dec = onePole(e.d / 6.9078f, sr);   // ln(1000): 60 dB of the way at `d`
    c.rel = onePole(e.r / 6.9078f, sr);
    c.sus = clampf(e.s, 0.0f, 1.0f);
    c.attN = std::pow(1.0f - c.att, static_cast<float>(kChunk));
    c.decN = std::pow(1.0f - c.dec, static_cast<float>(kChunk));
    c.relN = std::pow(1.0f - c.rel, static_cast<float>(kChunk));
    return c;
}

// --- notes --------------------------------------------------------------------------------

int Synth::voiceLimit() const {
    switch (patch_.voiceMode) {
        case VM_DUO: return 2;
        case VM_MONO:
        case VM_LEGATO: return 1;
        default: return patch_.voices;
    }
}

float Synth::shapeVelocity(int velocity) const {
    const float v = static_cast<float>(std::clamp(velocity, 1, 127)) / 127.0f;
    return std::pow(v, std::exp2(-2.0f * clampf(patch_.velCurve, -1.0f, 1.0f)));
}

void Synth::holdKey(int note, int velocity) {
    dropKey(note);
    if (nHeld_ == kHeldMax) {   // forget the oldest key
        for (int i = 1; i < kHeldMax; ++i) held_[i - 1] = held_[i];
        --nHeld_;
    }
    held_[nHeld_++] = {note, velocity};
}

void Synth::dropKey(int note) {
    int w = 0;
    for (int i = 0; i < nHeld_; ++i)
        if (held_[i].note != note) held_[w++] = held_[i];
    nHeld_ = w;
}

void Synth::noteOn(int note, int velocity) {
    if (velocity <= 0) {
        noteOff(note);
        return;
    }
    holdKey(note, velocity);
    if (patch_.voiceMode == VM_MONO || patch_.voiceMode == VM_LEGATO) noteOnMono(note, velocity);
    else noteOnPoly(note, velocity, voiceLimit());
}

void Synth::noteOnPoly(int note, int velocity, int limit) {
    Voice* v = nullptr;
    if (!patch_.sameNoteNew)   // the same note again: retrigger its voice, don't stack a second one
        for (auto& x : voices_)
            if (x.active && (x.pendingNote >= 0 ? x.pendingNote : x.note) == note) {
                v = &x;
                break;
            }
    if (v && v->fade > 0) {   // fading out for this very note: it starts after the fade, as planned
        v->pendingNote = note;
        v->pendingVel = velocity;
        v->pendingUp = false;
        return;
    }
    for (int i = 0; !v && i < limit; ++i)
        if (!voices_[i].active) v = &voices_[i];
    if (v) start(*v, note, velocity, nHeld_ > 1);
    else startOrSteal(*victim(limit), note, velocity);
}

// One voice: a new key while another is held moves the voice (Legato: no new attack;
// Mono: a new attack from where the envelopes are). Glide decides how the pitch moves.
void Synth::noteOnMono(int note, int velocity) {
    Voice& v = voices_[0];
    for (int i = 1; i < kMaxVoices; ++i)   // leftovers from a poly patch ring out
        if (voices_[i].active && (voices_[i].gate || voices_[i].sustained)) release(voices_[i]);
    const bool overlap = v.active && (v.gate || v.sustained) && v.pendingNote < 0;
    if (overlap && patch_.voiceMode == VM_LEGATO) {
        glideTo(v, note, true);
        v.gate = true;
        v.sustained = false;
        return;
    }
    start(v, note, velocity, nHeld_ > 1);
}

// All `limit` voices are sounding and a new note needs one of them. A voice already fading
// out for another new note is taken only when every voice is (a chord played into a full
// voice pool would otherwise keep only its last note).
Synth::Voice* Synth::victim(int limit) {
    limit = std::clamp(limit, 1, kMaxVoices);
    auto older = [](const Voice& a, const Voice& b) { return static_cast<int32_t>(a.age - b.age) < 0; };
    Voice* best = nullptr;
    for (int strict = 1; strict >= 0 && !best; --strict) {
        auto free = [&](const Voice& x) { return !strict || x.pendingNote < 0; };
        switch (patch_.steal) {
            case ST_QUIETEST: {
                // Released voices first (they are on their way out anyway), quietest first; else
                // the quietest held one.
                for (int pass = 0; pass < 2 && !best; ++pass)
                    for (int i = 0; i < limit; ++i) {
                        Voice& x = voices_[i];
                        const bool released = !x.gate && !x.sustained;
                        if ((pass == 0 && !released) || !free(x)) continue;
                        if (!best || x.env[0].v < best->env[0].v) best = &x;
                    }
                break;
            }
            case ST_KEEP_LOW:
            case ST_KEEP_HIGH: {
                // The oldest voice, except the one holding the lowest (highest) note: a bass line
                // (or a top melody) survives any chord played over it.
                Voice* keep = nullptr;
                for (int i = 0; i < limit; ++i) {
                    Voice& x = voices_[i];
                    if (!(x.gate || x.sustained)) continue;
                    const bool better = patch_.steal == ST_KEEP_LOW ? (!keep || x.note < keep->note) : (!keep || x.note > keep->note);
                    if (better) keep = &x;
                }
                for (int i = 0; i < limit; ++i)
                    if (&voices_[i] != keep && free(voices_[i]) && (!best || older(voices_[i], *best))) best = &voices_[i];
                break;
            }
            default:
                for (int i = 0; i < limit; ++i)
                    if (free(voices_[i]) && (!best || older(voices_[i], *best))) best = &voices_[i];
                break;
        }
    }
    return best ? best : &voices_[0];
}

// A stolen voice fades out for ~3 ms, then starts the new note from silence: no click.
void Synth::startOrSteal(Voice& v, int note, int velocity) {
    if (!v.active || v.env[0].v < 1e-3f) {
        v.active = false;
        start(v, note, velocity, nHeld_ > 1);
        return;
    }
    v.pendingNote = note;
    v.pendingVel = velocity;
    v.pendingUp = false;
    if (v.fade <= 0) v.fade = kFadeSamples;
    v.gate = false;
    v.sustained = false;
}

void Synth::glideTo(Voice& v, int note, bool legatoMove) {
    v.note = note;
    v.target = tuned(note);
    const bool glide = patch_.glideMode == GL_ALWAYS || (patch_.glideMode == GL_LEGATO && legatoMove);
    const float dist = std::fabs(v.target - v.pitch);
    if (!glide || dist < 1e-4f || patch_.glideTime <= 1e-4f) {
        v.pitch = v.target;
        v.glideLeft = 0;
    } else {   // counted in samples, not summed steps: a slow, small glide never stalls on rounding
        const float samples = patch_.glideTime * sr_ * (patch_.glideRate ? dist / 12.0f : 1.0f);
        v.glideFrom = v.pitch;
        v.glideLen = v.glideLeft = static_cast<int>(std::clamp(samples, 1.0f, 1e9f));
    }
    lastPitch_ = v.target;
    havePitch_ = true;
}

void Synth::start(Voice& v, int note, int velocity, bool legato) {
    const bool wasActive = v.active;
    // Where the pitch comes from: this voice's own pitch if it was sounding, else the last
    // note played (poly glide). "Legato" glides only while another key is held.
    v.pitch = wasActive ? v.pitch : (havePitch_ ? lastPitch_ : tuned(note));
    glideTo(v, note, legato);
    v.active = true;
    v.gate = true;
    v.sustained = false;
    v.pendingNote = -1;
    v.pendingUp = false;
    v.releaseIn = 0;
    v.fade = 0;
    v.age = ++clock_;
    v.vel = shapeVelocity(velocity);
    v.velGain = 1.0f - patch_.velSens + patch_.velSens * v.vel * v.vel;
    // Per-note modulation state: the Random and Alternate sources, retriggered LFOs, the
    // matrix modifiers' memories, LFO delay/fade timing.
    v.rnd = randomBipolar(v.rng);
    v.alt = alt_;
    alt_ = -alt_;
    v.sinceOn = 0;
    v.pressure = 0.0f;
    for (int l = 0; l < 2; ++l) {
        LfoState& st = v.lfo[l];
        if (patch_.lfo[l].trig == LT_RETRIG) st.phase = clampf(patch_.lfo[l].phase, 0.0f, 0.9999f);
        if (patch_.lfo[l].trig != LT_FREE || !wasActive) {
            st.held = randomBipolar(v.rng);
            st.from = randomBipolar(v.rng);
            st.to = randomBipolar(v.rng);
        }
    }
    for (int s = 0; s < kModSlots; ++s) {
        v.slotTimer[s] = 0.0f;   // S&H samples at once
        v.slotSlew[s] = 0.0f;
    }
    v.ownEnv = false;
    for (auto& e : v.env) e.stage = Attack;   // a retriggered voice attacks from where it is

    if (!wasActive) {
        for (auto& e : v.env) e.v = 0.0f;
        for (auto& f : v.svf)
            for (auto& ch : f)
                for (auto& st : ch) st = Svf{};
        v.combLive[0] = v.combLive[1] = false;   // a comb clears its lines when it first runs
        v.drift = 0.0f;
        resetPhases(v);
    }
}

// A fresh note's oscillator phases. Reset: the phase knob, unison voices spread around it by
// the golden ratio (a phase-aligned stack flanges; this one is the same on every note).
// Random: anywhere. Free: wherever the voice left them.
void Synth::resetPhases(Voice& v) {
    constexpr double kGolden = 0.6180339887498949;
    for (int o = 0; o < 2; ++o) {
        const OscPatch& p = patch_.osc[o];
        if (p.phaseMode == PH_FREE) continue;
        double base = clampf(p.phase, 0.0f, 1.0f);
        base -= std::floor(base);   // 360 degrees = 0 (2^32 doesn't fit the phase)
        for (int u = 0; u < kMaxUnison; ++u) {
            if (p.phaseMode == PH_RANDOM) {
                v.phase[o][u] = random(v.rng);
            } else {
                double ph = base + kGolden * u;
                ph -= std::floor(ph);
                v.phase[o][u] = static_cast<uint32_t>(ph * 4294967296.0);
            }
        }
        v.subPhase[o] = p.phaseMode == PH_RANDOM ? random(v.rng) : static_cast<uint32_t>(base * 4294967296.0);
    }
    v.noiseRng = random(v.rng) | 1u;
}

void Synth::noteOff(int note) {
    dropKey(note);
    for (auto& v : voices_)   // a stolen voice waiting for this note: it still plays, briefly
        if (v.active && v.pendingNote == note) v.pendingUp = true;

    if (patch_.voiceMode == VM_MONO || patch_.voiceMode == VM_LEGATO) {
        Voice& v = voices_[0];
        if (!(v.active && v.gate && v.note == note)) return;
        if (nHeld_ > 0) {   // back to the newest key still held
            const Held& k = held_[nHeld_ - 1];
            if (patch_.voiceMode == VM_LEGATO) glideTo(v, k.note, true);
            else start(v, k.note, k.vel, true);
            return;
        }
        v.gate = false;
        if (pedal_) v.sustained = true;
        else release(v);
        return;
    }

    for (auto& v : voices_) {
        if (!(v.active && v.gate && v.note == note)) continue;
        // Duo: a voice whose key went up takes over a held key that isn't sounding.
        if (patch_.voiceMode == VM_DUO && !pedal_) {
            int take = -1;
            for (int k = nHeld_ - 1; k >= 0 && take < 0; --k) {
                bool sounding = false;
                for (const auto& x : voices_)   // a voice fading out to start it counts as sounding it
                    sounding = sounding || (x.active && ((x.gate && x.note == held_[k].note) || x.pendingNote == held_[k].note));
                if (!sounding) take = k;
            }
            if (take >= 0) {
                glideTo(v, held_[take].note, true);
                continue;
            }
        }
        v.gate = false;
        if (pedal_) v.sustained = true;
        else release(v);
        if (patch_.sameNoteNew) break;   // stacked copies of a note go one key-up at a time
    }
}

void Synth::release(Voice& v) {
    v.gate = false;
    v.sustained = false;
    for (auto& e : v.env)
        if (e.stage != Idle) e.stage = Release;
}

void Synth::sustain(bool down) {
    pedal_ = down;
    if (!down)
        for (auto& v : voices_)
            if (v.active && v.sustained) release(v);
}

void Synth::pitchBend(float amount) { bend_ = clampf(amount, -1.0f, 1.0f); }

void Synth::allNotesOff() {
    nHeld_ = 0;
    for (auto& v : voices_) {
        v.pendingNote = -1;
        v.pendingUp = false;
        v.releaseIn = 0;
        if (v.active) release(v);
    }
}

void Synth::reset() {
    for (auto& v : voices_) {
        v.active = v.gate = v.sustained = v.pendingUp = false;
        v.pendingNote = -1;
        v.releaseIn = 0;
        v.fade = 0;
        v.glideLeft = 0;
        v.env[0] = v.env[1] = Env{};
    }
    nHeld_ = 0;
    pedal_ = false;
}

void Synth::controller(int cc, int value) {
    const float v = static_cast<float>(std::clamp(value, 0, 127)) / 127.0f;
    if (cc == 1) cc_[0] = v;
    else if (cc == 2) cc_[1] = v;
    else if (cc == 11) cc_[2] = v;
}

void Synth::aftertouch(float amount) { pressure_ = clampf(amount, 0.0f, 1.0f); }

void Synth::polyAftertouch(int note, float amount) {
    for (auto& v : voices_)
        if (v.active && v.note == note) v.pressure = clampf(amount, 0.0f, 1.0f);
}

void Synth::setTransport(double bpm, double beats, bool playing, bool beatsValid) {
    bpm_ = bpm > 1.0 ? bpm : 120.0;
    if (playing && beatsValid) beats_ = beats;   // stopped: keep counting on our own
    playing_ = playing;
}

void Synth::setSequencerSources(float seq, const float* shape4) {
    seqSrc_ = seq;
    for (int i = 0; i < 4; ++i) shapeSrc_[i] = shape4 ? shape4[i] : 0.0f;
}

float Synth::randomBipolar(uint32_t& state) { return static_cast<float>(static_cast<int32_t>(random(state))) * (1.0f / 2147483648.0f); }

// One LFO over one chunk: returns its value at the chunk's start (-1..1) and advances.
// `locked`: a synced Global LFO, its phase read from the song position (bars line up).
float Synth::lfoStep(LfoState& st, const LfoPatch& p, float rateMul, int n, bool locked, uint32_t& rng) {
    const double divBeats = kSyncBeats[std::clamp(p.div, 0, kNumSyncDivs - 1)];
    auto newCycle = [&] {
        st.held = randomBipolar(rng);
        st.from = st.to;
        st.to = randomBipolar(rng);
    };
    if (locked) {
        double ph = beats_ / divBeats + clampf(p.phase, 0.0f, 1.0f);
        ph -= std::floor(ph);
        if (static_cast<float>(ph) < st.phase) newCycle();
        st.phase = static_cast<float>(ph);
    }
    const float ph = st.phase;
    float w;
    switch (p.wave) {
        case LW_TRIANGLE: w = ph < 0.25f ? 4.0f * ph : (ph < 0.75f ? 2.0f - 4.0f * ph : 4.0f * ph - 4.0f); break;
        case LW_SAW_UP: w = 2.0f * ph - 1.0f; break;
        case LW_SAW_DOWN: w = 1.0f - 2.0f * ph; break;
        case LW_SQUARE: w = ph < 0.5f ? 1.0f : -1.0f; break;
        case LW_SAMPLE_HOLD: w = st.held; break;
        case LW_SMOOTH: w = st.from + (st.to - st.from) * (0.5f - 0.5f * std::cos(kPi * ph)); break;
        default: w = std::sin(2.0f * kPi * ph); break;
    }
    if (!locked) {
        const double hz = (p.sync ? bpm_ / 60.0 / divBeats : static_cast<double>(p.rateHz)) * rateMul;
        float next = ph + static_cast<float>(hz * n / sr_);
        if (next >= 1.0f) {
            next -= std::floor(next);
            newCycle();
        }
        st.phase = next;
    }
    st.out = w;
    return w;
}

// The voice's LFOs and the matrix for one chunk: fills `m` with every modulated value.
// Every field of `m` is written here (no zeroing before: this runs per voice per chunk).
void Synth::modulate(Voice& v, float env2, int n, Mods& m) {
    for (int o = 0; o < 2; ++o) {
        m.level[o] = patch_.osc[o].level;
        m.subLevel[o] = patch_.osc[o].subLevel;
    }
    m.noiseLevel = patch_.noise.level;
    m.noiseColor = patch_.noise.color;

    float lfo[2] = {};
    const float t = static_cast<float>(v.sinceOn) / sr_;
    for (int l = 0; l < 2; ++l) {
        if (!lfoUsed_[l]) continue;   // nothing listens: don't spend the cycles
        const LfoPatch& p = patch_.lfo[l];
        const float w = p.trig == LT_GLOBAL ? glfo_[l].out : lfoStep(v.lfo[l], p, v.lfoRateMul[l], n, false, v.rng);
        float fade = 1.0f;   // delay, then fade in (per voice, Global LFOs too)
        if (t < p.delay) fade = 0.0f;
        else if (p.fade > 0.0f) fade = std::min(1.0f, (t - p.delay) / p.fade);
        const float depth = clampf(p.depth + v.lfoDepthAdd[l], 0.0f, 1.0f);
        lfo[l] = (p.unipolar ? 0.5f * (w + 1.0f) : w) * depth * fade;
    }
    v.sinceOn += static_cast<uint32_t>(n);
    if (nSlots_ == 0) {
        for (int k = 0; k < 2; ++k) {
            m.pitch[k] = m.pos[k] = m.pan[k] = m.detune[k] = 0.0f;
            m.cutoff[k] = m.res[k] = m.drive[k] = 0.0f;
        }
        m.amp = 1.0f;
        m.voicePan = 0.0f;
        v.lfoRateMul[0] = v.lfoRateMul[1] = 1.0f;
        v.lfoDepthAdd[0] = v.lfoDepthAdd[1] = 0.0f;
        v.ownEnv = false;
        return;
    }

    float src[MS_COUNT];
    src[MS_NONE] = 0.0f;
    src[MS_ENV1] = v.env[0].v;
    src[MS_ENV2] = env2;
    src[MS_LFO1] = lfo[0];
    src[MS_LFO2] = lfo[1];
    src[MS_VELOCITY] = v.vel;
    src[MS_NOTE] = (static_cast<float>(v.note) - 60.0f) / 60.0f;
    src[MS_MODWHEEL] = cc_[0];
    src[MS_AFTERTOUCH] = std::max(pressure_, v.pressure);
    src[MS_BEND] = bend_;
    src[MS_RANDOM] = v.rnd;
    src[MS_ALTERNATE] = v.alt;
    src[MS_GATE] = v.gate || v.sustained ? 1.0f : 0.0f;
    src[MS_SEQ] = seqSrc_;
    for (int i = 0; i < 4; ++i) src[MS_SHAPE1 + i] = shapeSrc_[i];
    for (int i = 0; i < kXyAxes; ++i) src[MS_X1 + i] = clampf(patch_.xy[i], 0.0f, 1.0f);
    src[MS_BREATH] = cc_[1];
    src[MS_EXPRESSION] = cc_[2];
    src[MS_CONSTANT] = 1.0f;

    alignas(16) float acc[(MT_COUNT + 3) & ~3];   // cleared with vector stores (a memset call costs more)
    for (int i = 0; i < MT_COUNT; i += 4) store4(acc + i, splat(0.0f));
    for (int k = 0; k < nSlots_; ++k) {
        const int s = slots_[k];
        const ModSlot& ms = patch_.mod[s];
        float x = src[std::clamp(ms.src, 0, MS_COUNT - 1)];
        if (ms.via != MS_NONE) x *= src[std::clamp(ms.via, 0, MS_COUNT - 1)];
        const float a = clampf(ms.modAmt, -1.0f, 1.0f);
        switch (ms.mod) {
            case MM_CURVE:   // + toward exponential, - toward logarithmic
                if (a > 0.0f) x += a * (x * std::fabs(x) - x);
                else if (a < 0.0f) x += -a * ((x < 0.0f ? -1.0f : 1.0f) * std::sqrt(std::fabs(x)) - x);
                break;
            case MM_RECTIFY: x = std::fabs(x); break;
            case MM_QUANTIZE: {
                const float steps = 2.0f + std::round(std::fabs(a) * 14.0f);
                x = std::round(x * steps) / steps;
                break;
            }
            case MM_SAMPLE_HOLD:
                v.slotTimer[s] -= static_cast<float>(n);
                if (v.slotTimer[s] <= 0.0f) {
                    v.slotHold[s] = x;
                    v.slotTimer[s] += slotPeriod_[s];
                    if (v.slotTimer[s] <= 0.0f) v.slotTimer[s] = slotPeriod_[s];
                }
                x = v.slotHold[s];
                break;
            case MM_SLEW:
                v.slotSlew[s] += (x - v.slotSlew[s]) * chunkStep(slotSlewK_[s], n);
                x = v.slotSlew[s];
                break;
            default: break;
        }
        for (int j = 0; j < 2; ++j)
            if (ms.tgt[j] > MT_OFF && ms.tgt[j] < MT_COUNT) acc[ms.tgt[j]] += slotScale_[s][j] * x;
    }

    for (int o = 0; o < 2; ++o) {
        m.pitch[o] = acc[MT_PITCH] + acc[MT_O1_PITCH + o];
        m.pos[o] = acc[MT_O1_POS + o];
        m.level[o] = clampf(m.level[o] + acc[MT_O1_LEVEL + o], 0.0f, 1.0f);
        m.pan[o] = acc[MT_O1_PAN + o];
        m.detune[o] = acc[MT_O1_DETUNE + o];
        m.subLevel[o] = clampf(m.subLevel[o] + acc[MT_SUB1_LEVEL + o], 0.0f, 1.0f);
    }
    m.noiseLevel = clampf(m.noiseLevel + acc[MT_NOISE_LEVEL], 0.0f, 1.0f);
    m.noiseColor = clampf(m.noiseColor + acc[MT_NOISE_COLOR], -1.0f, 1.0f);
    for (int f = 0; f < 2; ++f) {
        m.cutoff[f] = acc[MT_F1_CUT + f] + acc[MT_CUT];
        m.res[f] = acc[MT_F1_RES + f];
        m.drive[f] = acc[MT_F1_DRIVE + f];
    }
    m.amp = clampf(1.0f + acc[MT_VOLUME], 0.0f, 2.0f);
    m.voicePan = acc[MT_PAN];
    for (int l = 0; l < 2; ++l) {
        v.lfoRateMul[l] = exp2Fast(clampf(acc[MT_L1_RATE + l], -8.0f, 8.0f));
        v.lfoDepthAdd[l] = acc[MT_L1_DEPTH + l];
    }
    v.ownEnv = envTargeted_;
    if (envTargeted_) {   // envelope stages: recompute this voice's coefficients when they moved
        for (int e = 0; e < 2; ++e) {
            const int base = e ? MT_E2_A : MT_E1_A;
            const float key[4] = {acc[base], acc[base + 1], acc[base + 2], acc[base + 3]};
            bool same = true;
            for (int i = 0; i < 4; ++i) same = same && std::fabs(key[i] - v.envKey[e][i]) < 2e-3f;
            if (same && v.envc[e].dec > 0.0f) continue;
            for (int i = 0; i < 4; ++i) v.envKey[e][i] = key[i];
            EnvPatch ep = patch_.env[e];
            ep.a *= std::exp2(key[0]);
            ep.d *= std::exp2(key[1]);
            ep.s = clampf(ep.s + key[2], 0.0f, 1.0f);
            ep.r *= std::exp2(key[3]);
            v.envc[e] = envCoef(ep, sr_);
        }
    }
}

Synth::VoiceInfo Synth::voiceInfo(int i) const {
    const Voice& v = voices_[std::clamp(i, 0, kMaxVoices - 1)];
    return {v.active, v.gate || v.pendingNote >= 0, v.pendingNote >= 0 ? v.pendingNote : v.note, v.pitch, v.env[0].v};
}

int Synth::activeVoices() const {
    int n = 0;
    for (const auto& v : voices_) n += v.active ? 1 : 0;
    return n;
}

uint32_t Synth::random(uint32_t& state) {   // xorshift32
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// --- audio --------------------------------------------------------------------------------

void Synth::render(float* outL, float* outR, int n) {
    float cutTarget[2];
    for (int f = 0; f < 2; ++f) cutTarget[f] = hzNote(patch_.flt[f].cutoffHz);

    for (int pos = 0; pos < n; pos += kChunk) {
        const int len = std::min(kChunk, n - pos);
        float* L = outL + pos;
        float* R = outR + pos;
        std::fill(L, L + len, 0.0f);
        std::fill(R, R + len, 0.0f);

        // Knob moves arrive in 1/128 steps: glide cutoff (~2 ms) and volume (~6 ms) between them.
        const float kCut = chunkStep(0.2f, len), kVol = chunkStep(0.07f, len);
        for (int f = 0; f < 2; ++f) cutSemi_[f] += (cutTarget[f] - cutSemi_[f]) * kCut;
        // The shared (Global) LFOs, then the song position moves on.
        for (int l = 0; l < 2; ++l)
            if (lfoUsed_[l] && patch_.lfo[l].trig == LT_GLOBAL) lfoStep(glfo_[l], patch_.lfo[l], 1.0f, len, patch_.lfo[l].sync, rng_);
        beats_ += bpm_ / 60.0 * static_cast<double>(len) / static_cast<double>(sr_);

        nLanes_ = 0;
        for (auto& v : voices_)
            if (v.active) ++nLanes_;
        if (nLanes_ > 0) {
            // A filter reads whole quads of lanes: clear them where a lane may not write. The
            // direct bus is only read by the lanes that wrote it; F2's input only needs clearing
            // when a source can go there (or F2 runs beside F1 on whatever is in it).
            const int wide = nLanes_ > 4 ? 8 : 4;   // floats per sample row in use
            bool toF2 = patch_.noise.route == RT_F2 || patch_.noise.route == RT_BOTH;
            for (const auto& o : patch_.osc) toF2 = toF2 || o.route == RT_F2 || o.route == RT_BOTH;
            for (int b = 0; b < 2; ++b) {
                if (b == 1 && !toF2 && !patch_.parallel) continue;   // serial below copies F1's output in
                for (auto* ch : bus_[b])
                    for (int i = 0; i < len * kMaxVoices; i += kMaxVoices) {
                        store4(ch + i, splat(0.0f));
                        if (wide == 8) store4(ch + i + 4, splat(0.0f));
                    }
            }
            int lane = 0;
            for (auto& v : voices_)
                if (v.active) prepareVoice(v, lane++, len);
            filterLanes(0, bus_[0][0], bus_[0][1], len);
            if (!patch_.parallel)   // serial: filter 1 feeds filter 2
                for (int ch = 0; ch < 2; ++ch) {
                    float* to = bus_[1][ch];
                    const float* from = bus_[0][ch];
                    for (int i = 0; i < len * kMaxVoices; i += kMaxVoices)
                        for (int q = 0; q < wide; q += 4)
                            store4(to + i + q, toF2 ? load4(to + i + q) + load4(from + i + q) : load4(from + i + q));
                }
            filterLanes(1, bus_[1][0], bus_[1][1], len);
            for (int k = 0; k < nLanes_; ++k) finishVoice(lanes_[k], k, L, R, len);
        }

        const float g0 = vol_;
        vol_ += (volTarget_ - vol_) * kVol;
        const float step = (vol_ - g0) / static_cast<float>(len);
        float g = g0;
        for (int i = 0; i < len; ++i) {
            g += step;
            L[i] *= g;
            R[i] *= g;
        }
    }
}

// Pass 1 for one voice: modulation, envelopes, pitch, and its sources into the three buses
// (column `lane`).
void Synth::prepareVoice(Voice& v, int lane, int n) {
    // Control rate: the mod envelope's value at the start of the chunk drives this chunk,
    // scaled by velocity as much as its VEL knob says; then the LFOs and the matrix.
    const float e2vel = clampf(patch_.env[1].vel, 0.0f, 1.0f);
    const float mod = v.env[1].v * (1.0f - e2vel + e2vel * v.vel);
    Lane& ln = lanes_[lane];   // built in place (Mods is ~100 bytes: no copies per voice per chunk)
    ln.v = &v;
    ln.mod = mod;
    ln.buses = 0;
    Mods& m = ln.m;
    modulate(v, mod, n, m);   // writes every field
    const EnvCoef& c1 = v.ownEnv ? v.envc[1] : envc_[1];
    envRun<false>(v.env[1], c1, patch_.env[1].loop && v.gate, nullptr, n);
    if (v.glideLeft > 0) {   // glide toward the note
        v.glideLeft = std::max(v.glideLeft - n, 0);
        v.pitch = v.target + (v.glideFrom - v.target) * static_cast<float>(v.glideLeft) / static_cast<float>(v.glideLen);
    }
    if (v.releaseIn > 0 && (v.releaseIn -= n) <= 0) {   // its key went up while it waited to start
        v.releaseIn = 0;
        if (v.gate) {
            v.gate = false;
            if (pedal_) v.sustained = true;
            else release(v);
        }
    }
    float pitch = v.pitch + (bend_ >= 0.0f ? bend_ * patch_.bendUp : bend_ * patch_.bendDown);
    if (patch_.engine != EN_CLEAN) {   // analog drift: a slow random walk, a few cents
        const float range = patch_.engine == EN_DIRTY ? 6.0f : 2.5f;
        const float r = randomBipolar(v.rng);
        const float w = n >= kChunk ? 1.0f : std::sqrt(static_cast<float>(n) / static_cast<float>(kChunk));   // a walk's step grows with sqrt(time)
        v.drift = clampf(v.drift * (1.0f - chunkStep(0.0005f, n)) + r * 0.08f * range * w, -range, range);
        pitch += v.drift * 0.01f;
    }

    for (int o = 0; o < 2; ++o) m.pos[o] += mod * patch_.env2Pos;

    // Three buses: into filter 1, into filter 2, and past both. Summed here (contiguous), then
    // stored once into this lane's column of each bus it uses.
    float loc[3][2][kChunk];
    auto bus = [&](int b) {   // a local bus, cleared on first use
        if (!(ln.buses & (1 << b))) {
            zeroChunk(loc[b][0]);
            zeroChunk(loc[b][1]);
            ln.buses |= 1 << b;
        }
        return loc[b];
    };
    float tl[kChunk], tr[kChunk];
    auto source = [&](int route, auto render) {
        if (route == RT_BOTH) {   // one signal into both filters
            zeroChunk(tl);
            zeroChunk(tr);
            render(tl, tr);
            for (int b = 0; b < 2; ++b) {
                float (*d)[kChunk] = bus(b);
                for (int i = 0; i < kChunk; i += 4) {
                    store4(d[0] + i, load4(d[0] + i) + load4(tl + i));
                    store4(d[1] + i, load4(d[1] + i) + load4(tr + i));
                }
            }
            return;
        }
        float (*d)[kChunk] = bus(route == RT_F2 ? 1 : (route == RT_DIRECT ? 2 : 0));
        render(d[0], d[1]);   // the renderers add into what is there
    };
    for (int o = 0; o < 2; ++o) {
        const bool osc = m.level[o] > 0.0f, sub = m.subLevel[o] > 0.0f;
        if (!osc && !sub) continue;
        source(patch_.osc[o].route, [&](float* l, float* r) {
            if (osc) renderOsc(v, o, pitch, m, l, r, n);
            if (sub) renderSub(v, o, pitch + m.pitch[o], m.subLevel[o], l, r, n);
        });
    }
    if (m.noiseLevel > 0.0f)
        source(patch_.noise.route, [&](float* l, float* r) {
            renderNoise(v.noiseRng, v.noiseLp, m.noiseColor, 0.5f * m.noiseLevel, l, r, n);
        });
    for (int b = 0; b < 3; ++b)
        if (ln.buses & (1 << b))
            for (int ch = 0; ch < 2; ++ch) {
                float* col = bus_[b][ch] + lane;
                for (int i = 0; i < n; ++i) col[i * kMaxVoices] = loc[b][ch][i];
            }
    ln.pitch = pitch;
}

// Pass 4 for one voice: its filtered buses, the voice pan, the amp envelope, a steal's fade.
void Synth::finishVoice(const Lane& l, int lane, float* outL, float* outR, int n) {
    Voice& v = *l.v;
    const Mods& m = l.m;
    // Voice pan (a matrix target): an equal-power balance on the voice's stereo output.
    float bl = 1.0f, br = 1.0f;
    if (m.voicePan != 0.0f) panGains(m.voicePan, bl, br);
    float tl[kChunk], tr[kChunk];
    for (int i = 0; i < n; ++i) {
        const int at = i * kMaxVoices + lane;
        tl[i] = bus_[1][0][at];
        tr[i] = bus_[1][1][at];
    }
    if (patch_.parallel)   // F1 beside F2
        for (int i = 0; i < n; ++i) {
            tl[i] += bus_[0][0][i * kMaxVoices + lane];
            tr[i] += bus_[0][1][i * kMaxVoices + lane];
        }
    if (l.buses & 4)       // past both filters
        for (int i = 0; i < n; ++i) {
            tl[i] += bus_[2][0][i * kMaxVoices + lane];
            tr[i] += bus_[2][1][i * kMaxVoices + lane];
        }
    if (bl != 1.0f || br != 1.0f)
        for (int i = 0; i < n; ++i) {
            tl[i] *= bl;
            tr[i] *= br;
        }

    const float vg = v.velGain * m.amp;
    const EnvCoef& c0 = v.ownEnv ? v.envc[0] : envc_[0];
    float amp[kChunk];
    envRun<true>(v.env[0], c0, false, amp, n);
    if (v.fade > 0) {   // being stolen: fade out, then start the waiting note
        constexpr float kInv = 1.0f / static_cast<float>(kFadeSamples);
        for (int i = 0; i < n; ++i) {
            const float a = amp[i] * vg * static_cast<float>(std::max(v.fade - i, 0)) * kInv;
            outL[i] += tl[i] * a;
            outR[i] += tr[i] * a;
        }
        v.fade -= n;
        if (v.fade <= 0) {
            v.fade = 0;
            v.active = false;
            if (v.pendingNote >= 0) {
                const bool up = v.pendingUp;
                start(v, v.pendingNote, v.pendingVel, nHeld_ > 1);
                if (up) v.releaseIn = kFadeSamples;   // a short key press still sounds, then releases
            }
        }
        return;
    }
    for (int i = 0; i < n; ++i) {
        const float a = amp[i] * vg;
        outL[i] += tl[i] * a;
        outR[i] += tr[i] * a;
    }
    if (v.env[0].stage == Idle) v.active = false;
}

// The hot loop. Phase is a 32-bit fixed-point fraction of a cycle: the top bits index the
// level's frame (its own length), the rest are the interpolation fraction, and wrap-around
// is free integer overflow (no floor(), no branch).
void Synth::renderOsc(Voice& v, int o, float pitch, const Mods& m, float* L, float* R, int n) const {
    const OscState& s = osc_[o];
    const OscPatch& p = patch_.osc[o];
    if (!s.table) {   // Noise: the position knob is its colour, unison doesn't apply
        const float level = m.level[o];
        renderNoise(v.noiseRng, v.oscNoiseLp[o], 2.0f * clampf(p.pos + m.pos[o], 0.0f, 1.0f) - 1.0f, 0.5f * level, L, R, n);
        return;
    }
    const Wavetable& t = *s.table;

    const float pos = clampf(p.pos + m.pos[o], 0.0f, 1.0f);
    const float fpos = pos * static_cast<float>(t.frames - 1);
    const int fa = std::min(static_cast<int>(fpos), std::max(t.frames - 2, 0));
    const int fb = std::min(fa + 1, t.frames - 1);
    const float morph = fpos - static_cast<float>(fa);

    // Detune modulated: this voice's unison ratios, from the stack's spread.
    float ratio[kMaxUnison];
    float maxRatio = s.maxRatio;
    if (m.detune[o] != 0.0f && s.n > 1) {
        const float det = clampf(p.detune + m.detune[o], 0.0f, 1.0f);
        const float oct = 100.0f * det * det / 1200.0f;
        for (int u = 0; u < s.n; ++u) ratio[u] = exp2Small(s.spread[u] * oct);
        maxRatio = exp2Small(oct);
    } else {
        for (int u = 0; u < s.n; ++u) ratio[u] = s.ratio[u];
    }
    const float inc = std::min(noteHz(pitch + p.pitch + m.pitch[o]) / sr_, 0.45f);   // cycles per sample
    const int mip = mipFor(inc * maxRatio);                                          // the stack's highest voice decides
    const float* A = t.get(fa, mip);
    const float* B = t.get(fb, mip);
    float lvl = m.level[o], lvr = m.level[o];
    if (m.pan[o] != 0.0f) {   // pan modulated: a balance on top of the stack's own placement
        float pl, pr;
        panGains(m.pan[o], pl, pr);
        lvl *= pl;
        lvr *= pr;
    }

    // This level's own length: the top `bits` of the phase index it, the rest interpolate.
    const int shift = 32 - kMipBits[mip];
    const uint32_t mask = (1u << shift) - 1;
    const float kFrac = 1.0f / static_cast<float>(1u << shift);
    const bool twoFrames = fb != fa && morph > 0.0f;   // else frame A alone (classic shapes, a position on a frame)
    for (int u = 0; u < s.n; ++u) {
        uint32_t ph = v.phase[o][u];
        const uint32_t dph = static_cast<uint32_t>(inc * ratio[u] * 4294967296.0f);
        const float gl = s.gl[u] * lvl, gr = s.gr[u] * lvr;
        int i = 0;
#ifdef PF_NEON
        // Four samples at a time. The four table positions are computed in core registers;
        // each (A[idx], A[idx+1]) pair is one 64-bit load, and an unzip splits the pairs into
        // "this sample" and "next sample" vectors. The fractions come from a phase vector.
        uint32x4_t phv = {ph, ph + dph, ph + 2 * dph, ph + 3 * dph};
        const uint32x4_t step4 = vdupq_n_u32(4 * dph), maskv = vdupq_n_u32(mask);
        const float32x4_t fracScale = vdupq_n_f32(kFrac), mv = vdupq_n_f32(morph);
        const float32x4_t glv = vdupq_n_f32(gl), grv = vdupq_n_f32(gr);
        for (; i + 4 <= n; i += 4) {
            const uint32_t i0 = ph >> shift, i1 = (ph + dph) >> shift, i2 = (ph + 2 * dph) >> shift, i3 = (ph + 3 * dph) >> shift;
            const float32x4_t fr = vmulq_f32(vcvtq_f32_u32(vandq_u32(phv, maskv)), fracScale);
            const float32x4x2_t pa = vuzpq_f32(vcombine_f32(vld1_f32(A + i0), vld1_f32(A + i1)),
                                               vcombine_f32(vld1_f32(A + i2), vld1_f32(A + i3)));
            float32x4_t x = vmlaq_f32(pa.val[0], fr, vsubq_f32(pa.val[1], pa.val[0]));
            if (twoFrames) {
                const float32x4x2_t pb = vuzpq_f32(vcombine_f32(vld1_f32(B + i0), vld1_f32(B + i1)),
                                                   vcombine_f32(vld1_f32(B + i2), vld1_f32(B + i3)));
                const float32x4_t b = vmlaq_f32(pb.val[0], fr, vsubq_f32(pb.val[1], pb.val[0]));
                x = vmlaq_f32(x, mv, vsubq_f32(b, x));
            }
            vst1q_f32(L + i, vmlaq_f32(vld1q_f32(L + i), x, glv));
            vst1q_f32(R + i, vmlaq_f32(vld1q_f32(R + i), x, grv));
            phv = vaddq_u32(phv, step4);
            ph += 4 * dph;
        }
#endif
        if (twoFrames) {
            for (; i < n; ++i) {
                const uint32_t idx = ph >> shift;
                const float fr = static_cast<float>(ph & mask) * kFrac;
                const float a = A[idx] + fr * (A[idx + 1] - A[idx]);
                const float b = B[idx] + fr * (B[idx + 1] - B[idx]);
                const float x = a + morph * (b - a);
                L[i] += x * gl;
                R[i] += x * gr;
                ph += dph;
            }
        } else {
            for (; i < n; ++i) {
                const uint32_t idx = ph >> shift;
                const float fr = static_cast<float>(ph & mask) * kFrac;
                const float x = A[idx] + fr * (A[idx + 1] - A[idx]);
                L[i] += x * gl;
                R[i] += x * gr;
                ph += dph;
            }
        }
        v.phase[o][u] = ph;
    }
}

// The sub oscillator: one classic-shape voice under the oscillator, panned with it.
void Synth::renderSub(Voice& v, int o, float pitch, float level, float* L, float* R, int n) const {
    const OscPatch& p = patch_.osc[o];
    const Wavetable& t = classicTable(std::clamp(p.subWave, 0, static_cast<int>(CW_SQUARE)));
    const float inc = std::min(noteHz(pitch + p.pitch + p.subTune) / sr_, 0.45f);
    const int mip = mipFor(inc);
    const float* A = t.get(0, mip);
    const int shift = 32 - kMipBits[mip];
    const uint32_t mask = (1u << shift) - 1;
    const float kFrac = 1.0f / static_cast<float>(1u << shift);
    const float gl = osc_[o].subGl * level, gr = osc_[o].subGr * level;
    uint32_t ph = v.subPhase[o];
    const uint32_t dph = static_cast<uint32_t>(inc * 4294967296.0f);
    int i = 0;
#ifdef PF_NEON
    uint32x4_t phv = {ph, ph + dph, ph + 2 * dph, ph + 3 * dph};
    const uint32x4_t step4 = vdupq_n_u32(4 * dph), maskv = vdupq_n_u32(mask);
    const float32x4_t fracScale = vdupq_n_f32(kFrac), glv = vdupq_n_f32(gl), grv = vdupq_n_f32(gr);
    for (; i + 4 <= n; i += 4) {   // as in renderOsc
        const uint32_t i0 = ph >> shift, i1 = (ph + dph) >> shift, i2 = (ph + 2 * dph) >> shift, i3 = (ph + 3 * dph) >> shift;
        const float32x4_t fr = vmulq_f32(vcvtq_f32_u32(vandq_u32(phv, maskv)), fracScale);
        const float32x4x2_t pa = vuzpq_f32(vcombine_f32(vld1_f32(A + i0), vld1_f32(A + i1)),
                                           vcombine_f32(vld1_f32(A + i2), vld1_f32(A + i3)));
        const float32x4_t x = vmlaq_f32(pa.val[0], fr, vsubq_f32(pa.val[1], pa.val[0]));
        vst1q_f32(L + i, vmlaq_f32(vld1q_f32(L + i), x, glv));
        vst1q_f32(R + i, vmlaq_f32(vld1q_f32(R + i), x, grv));
        phv = vaddq_u32(phv, step4);
        ph += 4 * dph;
    }
#endif
    for (; i < n; ++i) {
        const uint32_t idx = ph >> shift;
        const float fr = static_cast<float>(ph & mask) * kFrac;
        const float x = A[idx] + fr * (A[idx + 1] - A[idx]);
        L[i] += x * gl;
        R[i] += x * gr;
        ph += dph;
    }
    v.subPhase[o] = ph;
}

// Stereo noise with a colour tilt. Dark: a one-pole lowpass closing from white down to
// ~100 Hz. Bright: white plus up to 1.5x its own highpassed part (an upward tilt). Both
// continuous through white at 0, and scaled by the exact RMS of the filter on white noise
// so the colour knob changes the tone, not the level.
void Synth::renderNoise(uint32_t& rng, float* lp, float color, float gain, float* L, float* R, int n) const {
    color = clampf(color, -1.0f, 1.0f);
    constexpr float kB = 0.3f;   // the bright side's fixed lowpass (~2 kHz)
    float a, k, var;
    if (color <= 0.0f) {
        a = 1.0f - 0.985f * -color;
        k = 0.0f;
        var = a / (2.0f - a);
    } else {
        a = kB;
        k = 1.5f * color;
        var = (1.0f + k) * (1.0f + k) + k * k * kB / (2.0f - kB) - 2.0f * k * (1.0f + k) * kB;
    }
    const float g = gain * std::min(1.0f / std::sqrt(std::max(var, 1e-6f)), 12.0f);   // dark end needs 11.5
    constexpr float kScale = 1.7320508f / 2147483648.0f;   // uniform -1..1 has RMS 1/sqrt(3): make it 1
    for (int i = 0; i < n; ++i) {
        for (int ch = 0; ch < 2; ++ch) {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            const float w = static_cast<float>(static_cast<int32_t>(rng)) * kScale;
            lp[ch] += (w - lp[ch]) * a;
            const float x = color <= 0.0f ? lp[ch] : (1.0f + k) * w - k * lp[ch];
            (ch ? R : L)[i] += x * g;
        }
    }
}

// The formants of five vowels (Hz, an adult voice), morphed by the cutoff: A E I O U.
namespace {
constexpr float kVowels[5][3] = {
    {730.0f, 1090.0f, 2440.0f}, {530.0f, 1840.0f, 2480.0f}, {270.0f, 2290.0f, 3010.0f},
    {570.0f, 840.0f, 2410.0f},  {300.0f, 870.0f, 2240.0f},
};
constexpr float kFormantGain[3] = {1.0f, 0.63f, 0.4f};
}

void Synth::filterOne(Voice& v, int f, float pitch, const Mods& m, float mod, float* L, float* R, int n) const {
    const FilterPatch& p = patch_.flt[f];
    const Ctl c = controls(f, pitch, m, mod);
    const float hz = c.hz, res = c.res, drive = c.drive, pre = c.pre, post = c.post, wet = c.wet;
    auto drv = [&](float x) { return x + wet * (softclip(x * pre) * post - x); };
    const bool dirty = patch_.engine == EN_DIRTY;

    if (isComb(p.type)) {
        // A feedback comb tuned to the cutoff (keytrack 100% = the note's pitch): metallic
        // resonances, plucked-string tones. Comb- (inverted feedback) sounds an octave lower
        // and hollow. Resonance = feedback.
        const float delay = clampf(sr_ / hz, 2.0f, static_cast<float>(kCombLen - 2));
        const float fb = (0.25f + 0.72f * res) * (p.type == F_COMB_MINUS ? -1.0f : 1.0f);
        const float out = 1.0f - 0.55f * std::fabs(fb);
        const int d0 = static_cast<int>(delay);
        const float frac = delay - static_cast<float>(d0);
        constexpr int kMask = kCombLen - 1;
        if (!v.combLive[f]) {   // this note's first comb chunk: no older note in the lines
            std::fill_n(v.comb + static_cast<size_t>(f) * 2 * kCombLen, 2 * kCombLen, 0.0f);
            v.combLive[f] = true;
        }
        for (int ch = 0; ch < 2; ++ch) {
            float* x = ch ? R : L;
            float* line = v.comb + (static_cast<size_t>(f) * 2 + static_cast<size_t>(ch)) * kCombLen;
            int w = v.combPos;
            for (int i = 0; i < n; ++i) {
                const float in = drive > 0.0f ? drv(x[i]) : x[i];
                const float a = line[(w - d0) & kMask], b = line[(w - d0 - 1) & kMask];
                float y = in + fb * (a + frac * (b - a));
                if (dirty) y = softclip(y);
                line[w] = y;
                w = (w + 1) & kMask;
                x[i] = y * out;
            }
        }
        if (f == 1 || !isComb(patch_.flt[1].type))   // both filters share the write position
            v.combPos = (v.combPos + n) & (kCombLen - 1);
        return;
    }

    if (p.type == F_VOWEL) {
        // Three formant bandpasses; the cutoff (with its env/keytrack/modulation) walks A-E-I-O-U
        // across 20 Hz .. 20 kHz; resonance sharpens the formants.
        const float t = clampf(std::log2(hz / 20.0f) / 9.966f, 0.0f, 1.0f) * 4.0f;
        const int i0 = std::min(static_cast<int>(t), 3);
        const float u = t - static_cast<float>(i0);
        const float k = 0.9f - 0.8f * res;
        SvfCoef fc[3];
        for (int j = 0; j < 3; ++j) {
            const float fhz = kVowels[i0][j] + u * (kVowels[i0 + 1][j] - kVowels[i0][j]);
            fc[j] = makeSvf(tanFast(kPi * std::min(fhz, 0.45f * sr_) / sr_), k);
        }
        for (int ch = 0; ch < 2; ++ch) {
            float* x = ch ? R : L;
            for (int i = 0; i < n; ++i) {
                const float in = drive > 0.0f ? drv(x[i]) : x[i];
                float y = 0.0f, b, l;
                for (int j = 0; j < 3; ++j) {
                    svf(v.svf[f][ch][j], fc[j], in, b, l);
                    y += kFormantGain[j] * k * b;
                }
                x[i] = 1.6f * y;
            }
        }
    }
}

// A filter's controls for one voice this chunk: cutoff (smoothed) + env 2 + keytrack +
// modulation in semitones, resonance and drive.
Synth::Ctl Synth::controls(int f, float pitch, const Mods& m, float mod) const {
    const FilterPatch& p = patch_.flt[f];
    Ctl c;
    const float semi = cutSemi_[f] + p.env * mod * kEnvOctaves * 12.0f + p.key * (pitch - 60.0f) + m.cutoff[f];
    c.hz = clampf(noteHz(semi), 16.0f, 0.45f * sr_);
    c.res = clampf(p.res + m.res[f], 0.0f, 1.0f);
    c.drive = clampf(p.drive + m.drive[f], 0.0f, 1.0f);
    if (c.drive <= 0.0f) {   // no drive stage at all
        c.pre = c.post = 1.0f;
        c.wet = 0.0f;
        return c;
    }
    c.pre = 1.0f + 15.0f * c.drive * c.drive;
    c.post = 1.0f / softclip(c.pre);               // a full-scale input keeps ~unity gain
    c.wet = std::min(1.0f, 8.0f * c.drive);        // fades the drive in: no level step just above 0
    return c;
}

// Pass 2/3: one filter for every lane. The state-variable types run four voices at a time;
// combs and the vowel filter one voice at a time.
void Synth::filterLanes(int f, float* busL, float* busR, int n) {
    const FilterPatch& p = patch_.flt[f];
    if (!isComb(p.type))   // switched to a comb later: it starts from silence
        for (int k = 0; k < nLanes_; ++k) lanes_[k].v->combLive[f] = false;
    if (p.type == F_OFF) return;
    if (isComb(p.type) || p.type == F_VOWEL) {
        for (int k = 0; k < nLanes_; ++k) {
            float L[kChunk], R[kChunk];
            for (int i = 0; i < n; ++i) {
                L[i] = busL[i * kMaxVoices + k];
                R[i] = busR[i * kMaxVoices + k];
            }
            const Lane& l = lanes_[k];
            filterOne(*l.v, f, l.pitch, l.m, l.mod, L, R, n);
            for (int i = 0; i < n; ++i) {
                busL[i * kMaxVoices + k] = L[i];
                busR[i * kMaxVoices + k] = R[i];
            }
        }
        return;
    }
    const bool dirty = patch_.engine == EN_DIRTY;
    for (int q = 0; q < nLanes_; q += 4) {
        if (nLanes_ - q == 1) {   // one voice left: scalar
            const Lane& l = lanes_[q];
            const Ctl ctl = controls(f, l.pitch, l.m, l.mod);
            const float g = tanFast(kPi * ctl.hz / sr_);
            const SvfCoef c = makeSvf(g, 1.4142f - 1.36f * ctl.res), fl = makeSvf(g, 1.4142f);
            for (int ch = 0; ch < 2; ++ch) {
                float x[kChunk];
                float* col = (ch ? busR : busL) + q;
                for (int i = 0; i < n; ++i) x[i] = col[i * kMaxVoices];
                if (ctl.drive > 0.0f)
                    for (int i = 0; i < n; ++i) x[i] += ctl.wet * (softclip(x[i] * ctl.pre) * ctl.post - x[i]);
                if (dirty) svfScalar<true>(p.type, l.v->svf[f][ch][0], l.v->svf[f][ch][1], c, fl, x, n);
                else svfScalar<false>(p.type, l.v->svf[f][ch][0], l.v->svf[f][ch][1], c, fl, x, n);
                for (int i = 0; i < n; ++i) col[i * kMaxVoices] = x[i];
            }
            break;
        }
        // Lane j: its voice's coefficients and state; past the last voice a lane that passes
        // silence (g = 0) and is never written back.
        alignas(16) float k[4], a1[4], a2[4], a3[4], fk[4], fa1[4], fa2[4], fa3[4], pre[4], post[4], wet[4];
        alignas(16) float s[2][4][4];        // [channel][stage 1 ic1, ic2, stage 2 ic1, ic2][lane]
        bool drive = false;
        for (int j = 0; j < 4; ++j) {
            const int lane = q + j;
            SvfCoef c = makeSvf(0.0f, 1.4142f), fl = c;
            pre[j] = post[j] = 1.0f;
            wet[j] = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int x = 0; x < 4; ++x) s[ch][x][j] = 0.0f;
            if (lane < nLanes_) {
                const Lane& l = lanes_[lane];
                const Ctl ctl = controls(f, l.pitch, l.m, l.mod);
                const float g = tanFast(kPi * ctl.hz / sr_);
                c = makeSvf(g, 1.4142f - 1.36f * ctl.res);   // Q 0.7 .. ~18
                fl = makeSvf(g, 1.4142f);
                pre[j] = ctl.pre;
                post[j] = ctl.post;
                wet[j] = ctl.wet;
                drive = drive || ctl.drive > 0.0f;
                for (int ch = 0; ch < 2; ++ch) {
                    s[ch][0][j] = l.v->svf[f][ch][0].ic1;
                    s[ch][1][j] = l.v->svf[f][ch][0].ic2;
                    s[ch][2][j] = l.v->svf[f][ch][1].ic1;
                    s[ch][3][j] = l.v->svf[f][ch][1].ic2;
                }
            }
            k[j] = c.k;
            a1[j] = c.a1;
            a2[j] = c.a2;
            a3[j] = c.a3;
            fk[j] = fl.k;
            fa1[j] = fl.a1;
            fa2[j] = fl.a2;
            fa3[j] = fl.a3;
        }
        const SvfV cv{load4(k), load4(a1), load4(a2), load4(a3)};
        const SvfV fv{load4(fk), load4(fa1), load4(fa2), load4(fa3)};
        const QuadFn run = quadFor(p.type, dirty);
        for (int ch = 0; ch < 2; ++ch) {
            float* bus = (ch ? busR : busL) + q;
            if (drive) driveQuad(bus, n, DriveV{load4(pre), load4(post), load4(wet)});
            f4 st[4];
            for (int x = 0; x < 4; ++x) st[x] = load4(s[ch][x]);
            run(bus, n, cv, fv, st);
            for (int x = 0; x < 4; ++x) store4(s[ch][x], st[x]);
        }
        for (int j = 0; j < 4 && q + j < nLanes_; ++j) {
            Voice& v = *lanes_[q + j].v;
            for (int ch = 0; ch < 2; ++ch) {
                v.svf[f][ch][0].ic1 = s[ch][0][j];
                v.svf[f][ch][0].ic2 = s[ch][1][j];
                v.svf[f][ch][1].ic1 = s[ch][2][j];
                v.svf[f][ch][1].ic2 = s[ch][3][j];
            }
        }
    }
}

} // namespace pf
