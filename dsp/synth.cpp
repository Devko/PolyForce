#include "synth.h"

#include "simd.h"
#include "stages.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace pf {

#ifdef PF_STAGE_TIMING
uint64_t g_stageNs[STG_COUNT] = {};
#endif

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

// sin(x) for |x| <= pi/2 as a short odd series, error < 4e-6.
inline float sinQuarter(float x) {
    const float x2 = x * x;
    return x * (1.0f + x2 * (-1.666666667e-1f + x2 * (8.333333333e-3f + x2 * (-1.984126984e-4f + x2 * 2.755731922e-6f))));
}

// sin(2 pi x) for 0 <= x < 1 (the LFO sine), folded onto a quarter wave.
inline float sinCycle(float x) {
    const float y = x < 0.25f ? x : (x < 0.75f ? 0.5f - x : x - 1.0f);
    return sinQuarter(6.283185307f * y);
}

// Equal-power pan gains for a balance -1..1 (sqrt 2 at the sides, 1 in the middle).
inline void panGains(float pan, float& gl, float& gr) {
    const float a = (clampf(pan, -1.0f, 1.0f) + 1.0f) * 0.785398163f;   // 0 .. pi/2
    gl = sinQuarter(1.570796327f - a) * 1.41421356f;
    gr = sinQuarter(a) * 1.41421356f;
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

// A one-pole step `k` meant for 16 samples, over `n` samples (a chunk; render() also splits
// chunks at MIDI events and sequencer steps): the smoothing times don't depend on the chunk size.
inline float chunkStep(float k, int n) {
    if (n == 16) return k;
    if (n == 32) return k * (2.0f - k);   // 1 - (1 - k)^2
    return 1.0f - std::pow(1.0f - k, static_cast<float>(n) / 16.0f);
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

// The output's sum (Synth::finishLanes) over Q quads of lanes: F2's bus plus X more (F1's when
// parallel, the direct bus) times the amp envelope and the gliding gains, summed across the lanes.
// The envelope is c1 + c2 * q^(i+1) at sample i, worked out here, or (Tab: a steal's fade or a
// stage change inside the chunk) read from amp[]. Four samples' sums become one vector.
struct OutSum {
    const float* x[3][2];          // the buses to add up; left, right
    const float* amp;              // Tab: amp[i * kMaxVoices + lane]
    const float *c1, *c2, *q;      // else, per lane
    const float* g0[2];            // per lane: the gain before the chunk; sample i gets g0 + dg * (i + 1)
    const float* dg[2];
};

template <int Q, int X, bool Tab>
void sumLanes(const OutSum& s, float* outL, float* outR, int n) {
    f4 gl[Q], gr[Q], dl[Q], dr[Q], c1[Q], c2[Q], q[Q], p[Q];
    for (int j = 0; j < Q; ++j) {
        gl[j] = load4(s.g0[0] + 4 * j);
        gr[j] = load4(s.g0[1] + 4 * j);
        dl[j] = load4(s.dg[0] + 4 * j);
        dr[j] = load4(s.dg[1] + 4 * j);
        if (!Tab) {
            c1[j] = load4(s.c1 + 4 * j);
            c2[j] = load4(s.c2 + 4 * j);
            p[j] = q[j] = load4(s.q + 4 * j);
        }
    }
    // Locals: the output stores could otherwise alias `s`, and every pointer would be reloaded.
    const float *x0l = s.x[0][0], *x0r = s.x[0][1], *x1l = s.x[1][0], *x1r = s.x[1][1];
    const float *x2l = s.x[2][0], *x2r = s.x[2][1], *amp = s.amp;
    auto at = [&](int i, f4& sl, f4& sr) {   // sample i, still one lane per voice
        for (int j = 0; j < Q; ++j) {
            const int k = i * kMaxVoices + 4 * j;
            f4 xl = load4(x0l + k), xr = load4(x0r + k);
            if (X > 0) {
                xl += load4(x1l + k);
                xr += load4(x1r + k);
            }
            if (X > 1) {
                xl += load4(x2l + k);
                xr += load4(x2r + k);
            }
            f4 a;
            if (Tab) {
                a = load4(amp + k);
            } else {
                a = c1[j] + c2[j] * p[j];
                p[j] *= q[j];
            }
            gl[j] += dl[j];
            gr[j] += dr[j];
            const f4 yl = xl * (a * gl[j]), yr = xr * (a * gr[j]);
            sl = j ? sl + yl : yl;
            sr = j ? sr + yr : yr;
        }
    };
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        f4 l0, r0, l1, r1, l2, r2, l3, r3;
        at(i, l0, r0);
        at(i + 1, l1, r1);
        at(i + 2, l2, r2);
        at(i + 3, l3, r3);
        store4(outL + i, load4(outL + i) + hsum4x4(l0, l1, l2, l3));
        store4(outR + i, load4(outR + i) + hsum4x4(r0, r1, r2, r3));
    }
    for (; i < n; ++i) {   // a chunk cut short by an event
        f4 l, r;
        at(i, l, r);
        outL[i] += hsum4(l);
        outR[i] += hsum4(r);
    }
}

using SumFn = void (*)(const OutSum&, float*, float*, int);

// Two quads always read the table: working the envelopes out as well would need more than NEON's
// 16 q registers.
template <int Q, int X> SumFn sumOf(bool tab) {
    if constexpr (Q == 2) return sumLanes<Q, X, true>;
    else return tab ? sumLanes<Q, X, true> : sumLanes<Q, X, false>;
}

template <int Q> SumFn sumOf(int extra, bool tab) {
    return extra == 0 ? sumOf<Q, 0>(tab) : extra == 1 ? sumOf<Q, 1>(tab) : sumOf<Q, 2>(tab);
}

// One unison voice's samples from a wavetable level (Synth::renderOsc, renderSub), added into L and
// R: PK_FLOAT reads a float frame (the frame cache), PK_INT a 16-bit frame (its scale rides on the
// gains), PK_MORPH two 16-bit frames morphed. The phase is a 32-bit fixed-point fraction of a
// cycle: its top bits index the level (its own length), the rest are the interpolation fraction,
// and wrap-around is free integer overflow (no floor(), no branch). The gains glide: sample i gets
// g0 + dg * (i + 1), the last one the target. Returns the phase after the last sample.
enum PlayKind { PK_FLOAT, PK_INT, PK_MORPH };

// 1 / 2^(32 - bits) per level: the phase's fraction bits as 0..1, without a divide per call.
struct MipFrac {
    float v[kMipLevels];
};
constexpr MipFrac mipFrac() {
    MipFrac f{};
    for (int k = 0; k < kMipLevels; ++k) f.v[k] = 1.0f / static_cast<float>(1u << (32 - kMipBits[k]));
    return f;
}
constexpr MipFrac kMipFrac = mipFrac();

struct Play {
    const float* f = nullptr;                    // PK_FLOAT
    const int16_t *a = nullptr, *b = nullptr;    // PK_INT: a; PK_MORPH: a morphed toward b
    float sa = 1.0f, sb = 1.0f;                  // PK_MORPH: their scales
    float m0 = 0.0f, dm = 0.0f;                  // PK_MORPH: the morph at sample i is m0 + dm * (i + 1)
    int   shift = 32 - kTableBits;               // 32 - the level's bits
    float frac = kMipFrac.v[0];                  // 1 / 2^shift
    void level(int mip) {
        shift = 32 - kMipBits[mip];
        frac = kMipFrac.v[mip];
    }
};

#ifdef PF_NEON
// Four (p[i], p[i + 1]) pairs of 16-bit samples, one 32-bit load each: the first samples and the
// steps to the next, as floats.
inline void gather16(const int16_t* p, uint32_t i0, uint32_t i1, uint32_t i2, uint32_t i3, float32x4_t& x, float32x4_t& d) {
    uint32_t w0, w1, w2, w3;
    __builtin_memcpy(&w0, p + i0, 4);
    __builtin_memcpy(&w1, p + i1, 4);
    __builtin_memcpy(&w2, p + i2, 4);
    __builtin_memcpy(&w3, p + i3, 4);
    const int16x4x2_t z = vuzp_s16(vreinterpret_s16_u64(vcreate_u64(w0 | static_cast<uint64_t>(w1) << 32)),
                                   vreinterpret_s16_u64(vcreate_u64(w2 | static_cast<uint64_t>(w3) << 32)));
    x = vcvtq_f32_s32(vmovl_s16(z.val[0]));
    d = vcvtq_f32_s32(vsubl_s16(z.val[1], z.val[0]));
}
#endif

template <int Kind>
uint32_t play(const Play& p, uint32_t ph, uint32_t dph, float gl0, float dgl, float gr0, float dgr, float* L, float* R, int n) {
    const int shift = p.shift;
    const uint32_t mask = (1u << shift) - 1;
    const float kFrac = p.frac;
    const float* F = p.f;
    const int16_t *A = p.a, *B = p.b;
    int i = 0;
#ifdef PF_NEON
    // Four samples at a time. The four table positions are computed in core registers; each
    // (x[idx], x[idx+1]) pair is one load, and an unzip splits the pairs into "this sample" and
    // "next sample" vectors. The fractions come from a phase vector.
    uint32x4_t phv = {ph, ph + dph, ph + 2 * dph, ph + 3 * dph};
    const uint32x4_t step4 = vdupq_n_u32(4 * dph), maskv = vdupq_n_u32(mask);
    const float32x4_t fracScale = vdupq_n_f32(kFrac), one4 = {1.0f, 2.0f, 3.0f, 4.0f};
    float32x4_t mv = vmlaq_n_f32(vdupq_n_f32(p.m0), one4, p.dm);
    float32x4_t glv = vmlaq_n_f32(vdupq_n_f32(gl0), one4, dgl), grv = vmlaq_n_f32(vdupq_n_f32(gr0), one4, dgr);
    const float32x4_t dm4 = vdupq_n_f32(4.0f * p.dm), dgl4 = vdupq_n_f32(4.0f * dgl), dgr4 = vdupq_n_f32(4.0f * dgr);
    for (; i + 4 <= n; i += 4) {
        const uint32_t i0 = ph >> shift, i1 = (ph + dph) >> shift, i2 = (ph + 2 * dph) >> shift, i3 = (ph + 3 * dph) >> shift;
        const float32x4_t fr = vmulq_f32(vcvtq_f32_u32(vandq_u32(phv, maskv)), fracScale);
        float32x4_t x;
        if (Kind == PK_FLOAT) {
            const float32x4x2_t pa = vuzpq_f32(vcombine_f32(vld1_f32(F + i0), vld1_f32(F + i1)),
                                               vcombine_f32(vld1_f32(F + i2), vld1_f32(F + i3)));
            x = vmlaq_f32(pa.val[0], fr, vsubq_f32(pa.val[1], pa.val[0]));
        } else {
            float32x4_t a, da;
            gather16(A, i0, i1, i2, i3, a, da);
            x = vmlaq_f32(a, fr, da);
            if (Kind == PK_MORPH) {
                float32x4_t b, db;
                gather16(B, i0, i1, i2, i3, b, db);
                const float32x4_t xa = vmulq_n_f32(x, p.sa), xb = vmulq_n_f32(vmlaq_f32(b, fr, db), p.sb);
                x = vmlaq_f32(xa, mv, vsubq_f32(xb, xa));
                mv = vaddq_f32(mv, dm4);
            }
        }
        vst1q_f32(L + i, vmlaq_f32(vld1q_f32(L + i), x, glv));
        vst1q_f32(R + i, vmlaq_f32(vld1q_f32(R + i), x, grv));
        phv = vaddq_u32(phv, step4);
        ph += 4 * dph;
        glv = vaddq_f32(glv, dgl4);
        grv = vaddq_f32(grv, dgr4);
    }
#endif
    for (; i < n; ++i) {
        const float k = static_cast<float>(i + 1);
        const uint32_t idx = ph >> shift;
        const float fr = static_cast<float>(ph & mask) * kFrac;
        float x;
        if (Kind == PK_FLOAT) {
            x = F[idx] + fr * (F[idx + 1] - F[idx]);
        } else {
            const float a = static_cast<float>(A[idx]);
            x = a + fr * (static_cast<float>(A[idx + 1]) - a);
            if (Kind == PK_MORPH) {
                const float b = static_cast<float>(B[idx]);
                const float xa = x * p.sa, xb = (b + fr * (static_cast<float>(B[idx + 1]) - b)) * p.sb;
                x = xa + (p.m0 + p.dm * k) * (xb - xa);
            }
        }
        L[i] += x * (gl0 + dgl * k);
        R[i] += x * (gr0 + dgr * k);
        ph += dph;
    }
    return ph;
}

using PlayFn = uint32_t (*)(const Play&, uint32_t, uint32_t, float, float, float, float, float*, float*, int);
constexpr PlayFn kPlay[3] = {play<PK_FLOAT>, play<PK_INT>, play<PK_MORPH>};

} // namespace

Synth::Synth(float sampleRate) : sr_(sampleRate), invSr_(1.0f / sampleRate) {
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
        SlotRun r;
        r.slot = s;
        r.src = std::clamp(ms.src, 0, MS_COUNT - 1);
        r.via = ms.via == MS_NONE ? MS_CONSTANT : std::clamp(ms.via, 0, MS_COUNT - 1);
        r.mod = ms.mod;
        r.modAmt = clampf(ms.modAmt, -1.0f, 1.0f);
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
            const bool valid = ms.tgt[k] > MT_OFF && ms.tgt[k] < MT_COUNT;
            r.tgt[k] = valid ? ms.tgt[k] : MT_OFF;
            r.scale[k] = valid ? scale : 0.0f;
            live = live || (r.scale[k] != 0.0f && ms.src != MS_NONE);
            const Unit u = targetUnit(ms.tgt[k]);
            if (r.scale[k] != 0.0f && (u == Unit::EnvTime || u == Unit::EnvLevel)) envTargeted_ = true;
        }
        const float m = std::fabs(r.modAmt);
        slotSlewK_[s] = onePole(0.001f * std::exp2(m * 11.0f), sr_ / 16.0f);   // per 16 samples (chunkStep)
        slotPeriod_[s] = sr_ / (0.5f * std::exp2(m * 7.0f));
        if (live) runs_[nSlots_++] = r;
    }
    for (int l = 0; l < 2; ++l) {
        lfoUsed_[l] = false;
        for (int k = 0; k < nSlots_; ++k)
            lfoUsed_[l] = lfoUsed_[l] || runs_[k].src == MS_LFO1 + l || runs_[k].via == MS_LFO1 + l;
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
    if (v.active && v.fade > 0) {   // fading out (a steal, the CPU guard): the note starts after it
        v.pendingNote = note;
        v.pendingVel = velocity;
        v.pendingUp = false;
        return;
    }
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
        v.combFill[0] = v.combFill[1] = 0;   // a comb's lines start empty (older samples read as silence)
        v.ramp = Voice::Ramp{};                   // a fresh note's control values start where they are
        v.lfoRateMul[0] = v.lfoRateMul[1] = 1.0f;  // not the last note's matrix values for its first chunk
        v.lfoDepthAdd[0] = v.lfoDepthAdd[1] = 0.0f;
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
        case LW_SMOOTH: w = st.from + (st.to - st.from) * (0.5f - 0.5f * sinQuarter(kPi * (0.5f - ph))); break;   // cos(pi ph)
        default: w = sinCycle(ph); break;
    }
    if (!locked) {
        const float hz = (p.sync ? static_cast<float>(bpm_ / 60.0 / divBeats) : p.rateHz) * rateMul;
        float next = ph + hz * static_cast<float>(n) * invSr_;
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
    const float t = static_cast<float>(v.sinceOn) * invSr_;
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

    float* src = src_;   // the shared sources are in (render); now this voice's
    src[MS_ENV1] = v.env[0].v;
    src[MS_ENV2] = env2;
    src[MS_LFO1] = lfo[0];
    src[MS_LFO2] = lfo[1];
    src[MS_VELOCITY] = v.vel;
    src[MS_NOTE] = (static_cast<float>(v.note) - 60.0f) * (1.0f / 60.0f);
    src[MS_AFTERTOUCH] = std::max(pressure_, v.pressure);
    src[MS_RANDOM] = v.rnd;
    src[MS_ALTERNATE] = v.alt;
    src[MS_GATE] = v.gate || v.sustained ? 1.0f : 0.0f;

    alignas(16) float acc[(MT_COUNT + 3) & ~3];   // cleared with vector stores (a memset call costs more)
    for (int i = 0; i < MT_COUNT; i += 4) store4(acc + i, splat(0.0f));
    for (int k = 0; k < nSlots_; ++k) {
        const SlotRun& r = runs_[k];
        const int s = r.slot;
        float x = src[r.src] * src[r.via];
        const float a = r.modAmt;
        switch (r.mod) {
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
        acc[r.tgt[0]] += r.scale[0] * x;   // a dead target: MT_OFF, scale 0
        acc[r.tgt[1]] += r.scale[1] * x;
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

void Synth::waveView(int o, float* out, int n) const {
    if (n <= 0) return;
    const Wavetable* t = osc_[o].table;
    if (!t) {   // Noise
        uint32_t r = 0x2545F491u;
        for (int c = 0; c < n; ++c) out[c] = 0.8f * randomBipolar(r);
        return;
    }
    const float pos = clampf(patch_.osc[o].pos, 0.0f, 1.0f);   // as renderOsc, without the modulation
    const float fpos = pos * static_cast<float>(t->frames - 1);
    const int fa = std::min(static_cast<int>(fpos), std::max(t->frames - 2, 0));
    const int fb = std::min(fa + 1, t->frames - 1);
    const float morph = fpos - static_cast<float>(fa);
    const int16_t* A = t->get(fa, 0);
    const int16_t* B = t->get(fb, 0);
    const float sa = t->scale[static_cast<size_t>(fa)], sb = t->scale[static_cast<size_t>(fb)];
    const int len = mipLength(0);
    for (int c = 0; c < n; ++c) {
        float best = 0.0f;
        for (int s = c * len / n, e = (c + 1) * len / n; s < e; ++s) {
            const float a = static_cast<float>(A[s]) * sa;
            const float v = a + morph * (static_cast<float>(B[s]) * sb - a);
            if (std::fabs(v) > std::fabs(best)) best = v;
        }
        out[c] = clampf(best, -1.0f, 1.0f);
    }
}

int Synth::shedTails(int max) {
    int shed = 0;
    while (shed < max) {
        Voice* quiet = nullptr;
        for (auto& v : voices_)
            if (v.active && !v.gate && !v.sustained && v.fade <= 0 && v.pendingNote < 0 &&
                (!quiet || v.env[0].v < quiet->env[0].v))
                quiet = &v;
        if (!quiet) break;
        quiet->fade = kFadeSamples;   // finishLanes fades it, then frees it (nothing waits to start)
        ++shed;
    }
    return shed;
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
        cacheLeft_ = cacheOn_ ? kCacheBudget : 0;
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

        StageClock clock;   // the profiling build's pass timer (dsp/stages.h); else nothing
        nLanes_ = 0;
        for (auto& v : voices_)
            if (v.active) ++nLanes_;
        if (nLanes_ > 0 && nSlots_ > 0) {   // the matrix sources every voice shares (modulate adds its own)
            src_[MS_NONE] = 0.0f;
            src_[MS_MODWHEEL] = cc_[0];
            src_[MS_BEND] = bend_;
            src_[MS_SEQ] = seqSrc_;
            for (int i = 0; i < 4; ++i) src_[MS_SHAPE1 + i] = shapeSrc_[i];
            for (int i = 0; i < kXyAxes; ++i) src_[MS_X1 + i] = clampf(patch_.xy[i], 0.0f, 1.0f);
            src_[MS_BREATH] = cc_[1];
            src_[MS_EXPRESSION] = cc_[2];
            src_[MS_CONSTANT] = 1.0f;
        }
        if (nLanes_ > 0) {
            // The filters and the output read whole quads of lanes: clear them where a lane may not
            // write. F2's input only needs clearing when a source can go there (or F2 runs beside
            // F1 on whatever is in it), the direct bus when a source is routed there.
            const int wide = nLanes_ > 4 ? 8 : 4;   // floats per sample row in use
            bool toF2 = patch_.noise.route == RT_F2 || patch_.noise.route == RT_BOTH;
            bool direct = patch_.noise.route == RT_DIRECT;
            for (const auto& o : patch_.osc) {
                toF2 = toF2 || o.route == RT_F2 || o.route == RT_BOTH;
                direct = direct || o.route == RT_DIRECT;
            }
            for (int b = 0; b < 3; ++b) {
                if (b == 1 && !toF2 && !patch_.parallel) continue;   // serial below copies F1's output in
                if (b == 2 && !direct) continue;                     // the output reads every lane's direct bus
                for (auto* ch : bus_[b])
                    for (int i = 0; i < len * kMaxVoices; i += kMaxVoices) {
                        store4(ch + i, splat(0.0f));
                        if (wide == 8) store4(ch + i + 4, splat(0.0f));
                    }
            }
            int lane = 0;
            for (auto& v : voices_)
                if (v.active) controlVoice(v, lane++, len);
            clock.lap(STG_CONTROL);
            for (int k = 0; k < nLanes_; ++k) renderSources(k, len);
            clock.lap(STG_SOURCES);
            filterLanes(0, bus_[0][0], bus_[0][1], len);
            clock.lap(STG_FILTER1);
            if (!patch_.parallel)   // serial: filter 1 feeds filter 2
                for (int ch = 0; ch < 2; ++ch) {
                    float* to = bus_[1][ch];
                    const float* from = bus_[0][ch];
                    for (int i = 0; i < len * kMaxVoices; i += kMaxVoices)
                        for (int q = 0; q < wide; q += 4)
                            store4(to + i + q, toF2 ? load4(to + i + q) + load4(from + i + q) : load4(from + i + q));
                }
            filterLanes(1, bus_[1][0], bus_[1][1], len);
            clock.lap(STG_FILTER2);
            finishLanes(L, R, len, direct);
            clock.lap(STG_OUTPUT);
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

// Pass 1a for one voice: modulation, the mod envelope, glide, pitch: its Lane.
void Synth::controlVoice(Voice& v, int lane, int n) {
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
        const float w = n == 32 ? 1.41421356f : std::sqrt(static_cast<float>(n) / 16.0f);   // a walk's step grows with sqrt(time)
        v.drift = clampf(v.drift * (1.0f - chunkStep(0.0005f, n)) + r * 0.08f * range * w, -range, range);
        pitch += v.drift * 0.01f;
    }

    for (int o = 0; o < 2; ++o) m.pos[o] += mod * patch_.env2Pos;
    ln.pitch = pitch;
}

// Pass 1b for one voice: its oscillators, subs and noise into the three buses (column `lane`):
// into filter 1, into filter 2, and past both. Summed here (contiguous), then stored once into
// this lane's column of each bus it uses.
void Synth::renderSources(int lane, int n) {
    Lane& ln = lanes_[lane];
    Voice& v = *ln.v;
    const Mods& m = ln.m;
    const float pitch = ln.pitch;
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
    // A source turned down to 0 still renders one chunk, gliding to silence from where the last
    // one ended; after that it is skipped until it comes back, fading in from 0.
    const bool was = v.ramp.valid;
    for (int o = 0; o < 2; ++o) {
        const bool osc = m.level[o] > 0.0f ||
                         (was && (v.ramp.oscGain[o][0] != 0.0f || v.ramp.oscGain[o][1] != 0.0f || v.ramp.oscNoiseGain[o] != 0.0f));
        const bool sub = m.subLevel[o] > 0.0f || (was && v.ramp.subGain[o] != 0.0f);
        if (!osc) {   // silent: a later chunk fades in from silence
            v.ramp.oscGain[o][0] = v.ramp.oscGain[o][1] = v.ramp.oscNoiseGain[o] = 0.0f;
            v.ramp.frame[o] = -1;
        }
        if (!sub) v.ramp.subGain[o] = 0.0f;
        if (!osc && !sub) continue;
        source(patch_.osc[o].route, [&](float* l, float* r) {
            if (osc) renderOsc(v, o, pitch, m, l, r, n);
            if (sub) renderSub(v, o, pitch + m.pitch[o], m.subLevel[o], l, r, n);
        });
    }
    if (m.noiseLevel > 0.0f || (was && v.ramp.noiseGain != 0.0f))
        source(patch_.noise.route, [&](float* l, float* r) {
            renderNoise(v.noiseRng, v.noiseLp, m.noiseColor, 0.5f * m.noiseLevel, v.ramp.noiseGain, v.ramp.valid, l, r, n);
        });
    else
        v.ramp.noiseGain = 0.0f;
    for (int b = 0; b < 3; ++b)
        if (ln.buses & (1 << b))
            for (int ch = 0; ch < 2; ++ch) {
                float* col = bus_[b][ch] + lane;
                for (int i = 0; i < n; ++i) col[i * kMaxVoices] = loc[b][ch][i];
            }
}

// Pass 4: every lane's filtered signal (F2's bus; F1's beside it when parallel; the direct bus) times
// its amp envelope, steal fade and gains, summed into the output four voices per vector (sumLanes).
// An envelope with no stage change inside the chunk is c1 + c2 * q^(i+1) at sample i (attack toward
// 1.2, decay toward the sustain, release toward 0), worked out in the sum itself. A chunk where some
// lane changes stage (the attack reaching 1, the release ending: envRun sample by sample) or fades
// for a steal writes every envelope to a table first. Then each voice's own bookkeeping: a steal's
// fade ending starts the waiting note, an ended release frees the voice.
void Synth::finishLanes(float* outL, float* outR, int n, bool direct) {
    const int wide = nLanes_ > 4 ? 8 : 4;   // lanes in use, in whole quads (the rest: amp 0)
    alignas(16) float amp[kChunk * kMaxVoices];
    alignas(16) float c1[kMaxVoices] = {}, c2[kMaxVoices] = {}, q[kMaxVoices];
    alignas(16) float g0[2][kMaxVoices] = {}, dg[2][kMaxVoices] = {};
    bool own[kMaxVoices] = {};   // its amp column comes from envRun (a stage change inside the chunk)
    bool tab = false;            // some lane needs the amp table (own, or a steal's fade)
    std::fill(q, q + kMaxVoices, 1.0f);
    const float invN = 1.0f / static_cast<float>(n);
    for (int k = 0; k < nLanes_; ++k) {
        const Lane& l = lanes_[k];
        Voice& v = *l.v;
        const Mods& m = l.m;
        // Voice pan (a matrix target): an equal-power balance on the voice's stereo output. With
        // the velocity and the matrix's volume it glides in from the last chunk's.
        float bl = 1.0f, br = 1.0f;
        if (m.voicePan != 0.0f) panGains(m.voicePan, bl, br);
        const float vg = v.velGain * m.amp;
        const float gl1 = vg * bl, gr1 = vg * br;
        const float gl0 = v.ramp.valid ? v.ramp.outGain[0] : gl1, gr0 = v.ramp.valid ? v.ramp.outGain[1] : gr1;
        v.ramp.outGain[0] = gl1;
        v.ramp.outGain[1] = gr1;
        v.ramp.valid = true;   // this chunk's values are the next one's starting points
        g0[0][k] = gl0;
        g0[1][k] = gr0;
        dg[0][k] = (gl1 - gl0) * invN;
        dg[1][k] = (gr1 - gr0) * invN;

        Env& e = v.env[0];
        const EnvCoef& c = v.ownEnv ? v.envc[0] : envc_[0];
        float a1 = 0.0f, a2 = 0.0f, qq = 1.0f, qn = 1.0f;   // qn = qq^n: what is left after the chunk
        switch (e.stage) {
            case Attack: a1 = 1.2f, a2 = e.v - 1.2f, qq = 1.0f - c.att, qn = c.attN; break;
            case Decay: a1 = c.sus, a2 = e.v - c.sus, qq = 1.0f - c.dec, qn = c.decN; break;
            case Release: a2 = e.v, qq = 1.0f - c.rel, qn = c.relN; break;
            case Idle: break;
        }
        if (n != kChunk) {   // a chunk cut short by an event
            qn = 1.0f;
            for (int i = 0; i < n; ++i) qn *= qq;
        }
        const float end = a1 + a2 * qn;
        tab = tab || v.fade > 0;
        if ((e.stage == Attack && end >= 1.0f) || (e.stage == Release && end < 1e-4f)) {
            own[k] = tab = true;
            continue;
        }
        c1[k] = a1;
        c2[k] = a2;
        q[k] = qq;
        e.v = end;
    }
    OutSum sum;
    sum.amp = amp;
    sum.c1 = c1;
    sum.c2 = c2;
    sum.q = q;
    for (int ch = 0; ch < 2; ++ch) {
        sum.g0[ch] = g0[ch];
        sum.dg[ch] = dg[ch];
        int x = 0;
        sum.x[x++][ch] = bus_[1][ch];
        if (patch_.parallel) sum.x[x++][ch] = bus_[0][ch];
        if (direct) sum.x[x++][ch] = bus_[2][ch];
        for (; x < 3; ++x) sum.x[x][ch] = nullptr;
    }
    const int extra = (patch_.parallel ? 1 : 0) + (direct ? 1 : 0);
    tab = tab || wide == 8;
    if (tab) {   // a steal's fade or a stage change in this chunk, or five voices or more: a table
        for (int j = 0; j < wide; j += 4) {
            const f4 C1 = load4(c1 + j), C2 = load4(c2 + j), Q = load4(q + j);
            f4 P = Q;
            for (int i = 0; i < n; ++i) {
                store4(amp + i * kMaxVoices + j, C1 + C2 * P);
                P *= Q;
            }
        }
        constexpr float kInv = 1.0f / static_cast<float>(kFadeSamples);
        for (int k = 0; k < nLanes_; ++k) {
            Voice& v = *lanes_[k].v;
            if (own[k]) {
                float a[kChunk];
                envRun<true>(v.env[0], v.ownEnv ? v.envc[0] : envc_[0], false, a, n);
                for (int i = 0; i < n; ++i) amp[i * kMaxVoices + k] = a[i];
            }
            if (v.fade > 0)   // being stolen: fading out over kFadeSamples
                for (int i = 0; i < n; ++i) amp[i * kMaxVoices + k] *= static_cast<float>(std::max(v.fade - i, 0)) * kInv;
        }
    }
    (wide == 8 ? sumOf<2>(extra, tab) : sumOf<1>(extra, tab))(sum, outL, outR, n);

    for (int k = 0; k < nLanes_; ++k) {
        Voice& v = *lanes_[k].v;
        if (v.fade > 0) {   // being stolen: when the fade is over, the waiting note starts
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
            continue;
        }
        if (v.env[0].stage == Idle) v.active = false;
    }
}

// The frame cache (see FrameSlot): the float copy of level `mip` of frame fa (fb == fa) or of fa
// and fb premixed at `morph`, found or filled; null when it would have to be filled and this
// chunk's budget is spent. `hint`: the slot the caller found last time, tried first (inline: the
// usual case); findFrame searches the rest and fills.
inline const float* Synth::cachedFrame(const Wavetable& t, int fa, int fb, float morph, int mip, int& hint) {
    if (static_cast<unsigned>(hint) < static_cast<unsigned>(kCacheSlots) && cache_[hint].holds(t.id, fa, fb, mip, morph)) {
        cache_[hint].used = ++cacheClock_;
        return cache_[hint].data;
    }
    return findFrame(t, fa, fb, morph, mip, hint);
}

const float* Synth::findFrame(const Wavetable& t, int fa, int fb, float morph, int mip, int& hint) {
    int lru = 0;
    for (int k = 0; k < kCacheSlots; ++k) {
        if (cache_[k].holds(t.id, fa, fb, mip, morph)) {
            hint = k;
            cache_[k].used = ++cacheClock_;
            return cache_[k].data;
        }
        if (cache_[k].used < cache_[lru].used) lru = k;
    }
    const int len = mipLength(mip) + 1;   // with the guard sample
    if (cacheLeft_ < len) return nullptr;
    cacheLeft_ -= len;
    FrameSlot& c = cache_[lru];
    const int16_t* a = t.get(fa, mip);
    if (fb == fa) {
        const float sa = t.scale[static_cast<size_t>(fa)];
        for (int i = 0; i < len; ++i) c.data[i] = static_cast<float>(a[i]) * sa;
    } else {
        const int16_t* b = t.get(fb, mip);
        const float wa = t.scale[static_cast<size_t>(fa)] * (1.0f - morph), wb = t.scale[static_cast<size_t>(fb)] * morph;
        for (int i = 0; i < len; ++i) c.data[i] = static_cast<float>(a[i]) * wa + static_cast<float>(b[i]) * wb;
    }
    c.id = t.id;
    c.fa = fa;
    c.fb = fb;
    c.mip = mip;
    c.morph = morph;
    c.used = ++cacheClock_;
    hint = lru;
    return c.data;
}

// One oscillator of one voice: its unison stack at this chunk's pitch and position (play()).
void Synth::renderOsc(Voice& v, int o, float pitch, const Mods& m, float* L, float* R, int n) {
    const OscState& s = osc_[o];
    const OscPatch& p = patch_.osc[o];
    if (!s.table) {   // Noise: the position knob is its colour, unison doesn't apply
        v.ramp.oscGain[o][0] = v.ramp.oscGain[o][1] = 0.0f;   // a later switch back to a table fades in
        v.ramp.frame[o] = -1;
        const float level = m.level[o];
        renderNoise(v.noiseRng, v.oscNoiseLp[o], 2.0f * clampf(p.pos + m.pos[o], 0.0f, 1.0f) - 1.0f, 0.5f * level,
                    v.ramp.oscNoiseGain[o], v.ramp.valid, L, R, n);
        return;
    }
    const Wavetable& t = *s.table;
    v.ramp.oscNoiseGain[o] = 0.0f;   // a later switch to Noise fades in

    const float pos = clampf(p.pos + m.pos[o], 0.0f, 1.0f);
    const float fpos = pos * static_cast<float>(t.frames - 1);
    const int fa = std::min(static_cast<int>(fpos), std::max(t.frames - 2, 0));
    const int fb = std::min(fa + 1, t.frames - 1);
    const float morph = fpos - static_cast<float>(fa);
    // The morph glides between the same two frames (a new pair: it jumps, the frames differ anyway).
    const float morph0 = v.ramp.valid && v.ramp.frame[o] == fa ? v.ramp.morph[o] : morph;
    const bool still = v.ramp.valid && v.ramp.frame[o] == fa && morph0 == morph;   // where it was last chunk
    v.ramp.morph[o] = morph;
    v.ramp.frame[o] = fa;
    const float invN = 1.0f / static_cast<float>(n);
    const float dMorph = (morph - morph0) * invN;

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
    float lvl = m.level[o], lvr = m.level[o];
    if (m.pan[o] != 0.0f) {   // pan modulated: a balance on top of the stack's own placement
        float pl, pr;
        panGains(m.pan[o], pl, pr);
        lvl *= pl;
        lvr *= pr;
    }
    const float lvl0 = v.ramp.valid ? v.ramp.oscGain[o][0] : lvl, lvr0 = v.ramp.valid ? v.ramp.oscGain[o][1] : lvr;
    v.ramp.oscGain[o][0] = lvl;
    v.ramp.oscGain[o][1] = lvr;

    const bool twoFrames = fb != fa && (morph > 0.0f || morph0 > 0.0f);   // else frame A alone (classic shapes, a position on a frame)
    // A position that holds still (the same as last chunk) plays the frame cache's float copy, two
    // frames premixed; a moving one, a note's first chunk or the cache's budget spent reads the
    // 16-bit frames as it goes (a frame pair just reached is not worth a copy: the position moves).
    Play src;
    src.level(mip);
    int kind = PK_FLOAT;
    float gs = 1.0f;   // PK_INT: the frame's scale, on the gains
    if (!still || !(src.f = cachedFrame(t, fa, twoFrames ? fb : fa, twoFrames ? morph : 0.0f, mip, v.cacheSlot[o]))) {
        src.a = t.get(fa, mip);
        if (twoFrames) {
            kind = PK_MORPH;
            src.b = t.get(fb, mip);
            src.sa = t.scale[static_cast<size_t>(fa)];
            src.sb = t.scale[static_cast<size_t>(fb)];
            src.m0 = morph0;
            src.dm = dMorph;
        } else {
            kind = PK_INT;
            gs = t.scale[static_cast<size_t>(fa)];
        }
    }
    for (int u = 0; u < s.n; ++u) {
        const uint32_t dph = static_cast<uint32_t>(inc * ratio[u] * 4294967296.0f);
        const float gl0 = s.gl[u] * lvl0, gr0 = s.gr[u] * lvr0;
        const float dgl = (s.gl[u] * lvl - gl0) * invN, dgr = (s.gr[u] * lvr - gr0) * invN;
        v.phase[o][u] = kPlay[kind](src, v.phase[o][u], dph, gl0 * gs, dgl * gs, gr0 * gs, dgr * gs, L, R, n);
    }
}

// The sub oscillator: one classic-shape voice under the oscillator, panned with it.
void Synth::renderSub(Voice& v, int o, float pitch, float level, float* L, float* R, int n) {
    const OscPatch& p = patch_.osc[o];
    const Wavetable& t = classicTable(std::clamp(p.subWave, 0, static_cast<int>(CW_SQUARE)));
    const float inc = std::min(noteHz(pitch + p.pitch + p.subTune) / sr_, 0.45f);
    const int mip = mipFor(inc);
    const float level0 = v.ramp.valid ? v.ramp.subGain[o] : level;
    v.ramp.subGain[o] = level;
    const float invN = 1.0f / static_cast<float>(n);
    const float gl0 = osc_[o].subGl * level0, gr0 = osc_[o].subGr * level0;
    const float dgl = (osc_[o].subGl * level - gl0) * invN, dgr = (osc_[o].subGr * level - gr0) * invN;
    const uint32_t dph = static_cast<uint32_t>(inc * 4294967296.0f);
    Play src;
    src.level(mip);
    src.f = cachedFrame(t, 0, 0, 0.0f, mip, v.cacheSlot[2 + o]);
    if (src.f) {
        v.subPhase[o] = play<PK_FLOAT>(src, v.subPhase[o], dph, gl0, dgl, gr0, dgr, L, R, n);
    } else {
        src.a = t.get(0, mip);
        const float gs = t.scale[0];
        v.subPhase[o] = play<PK_INT>(src, v.subPhase[o], dph, gl0 * gs, dgl * gs, gr0 * gs, dgr * gs, L, R, n);
    }
}


// Stereo noise with a colour tilt. Dark: a one-pole lowpass closing from white down to
// ~100 Hz. Bright: white plus up to 1.5x its own highpassed part (an upward tilt). Both
// continuous through white at 0, and scaled by the exact RMS of the filter on white noise
// so the colour knob changes the tone, not the level.
void Synth::renderNoise(uint32_t& rng, float* lp, float color, float gain, float& prevGain, bool ramped, float* L, float* R, int n) const {
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
    const float g1 = gain * std::min(1.0f / std::sqrt(std::max(var, 1e-6f)), 12.0f);   // dark end needs 11.5
    const float g0 = ramped ? prevGain : g1, dg = (g1 - g0) / static_cast<float>(n);
    prevGain = g1;
    constexpr float kScale = 1.7320508f / 2147483648.0f;   // uniform -1..1 has RMS 1/sqrt(3): make it 1
    float g = g0;
    for (int i = 0; i < n; ++i) {
        g += dg;
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
    const float hz = c.hz, res = c.res;
    const bool dirty = patch_.engine == EN_DIRTY;
    // Like the state-variable types (filterLanes), the controls glide from the last chunk's.
    auto glideFrom = [&](const float* to, int nc, bool same, float* from) {
        const bool glide = v.ramp.valid && same;
        std::copy(glide ? v.ramp.coef[f] : to, (glide ? v.ramp.coef[f] : to) + nc, from);
        std::copy(to, to + nc, v.ramp.coef[f]);
        v.ramp.fType[f] = p.type;
    };

    if (isComb(p.type)) {
        // A feedback comb tuned to the cutoff (keytrack 100% = the note's pitch): metallic
        // resonances, plucked-string tones. Comb- (inverted feedback) sounds an octave lower
        // and hollow. Resonance = feedback.
        const float delay = clampf(sr_ / hz, 2.0f, static_cast<float>(kCombLen - 2));
        const float fb = (0.25f + 0.72f * res) * (p.type == F_COMB_MINUS ? -1.0f : 1.0f);
        constexpr int kMask = kCombLen - 1;
        // Not cleared when a note starts (2 x 16 KB per filter, 0.5 MB for a chord on both combs in
        // one chunk): until the line is full, a tap further back than this note has written reads 0.
        const int fill = v.combFill[f];
        const bool live = fill > 0;
        // Delay, feedback and drive move per sample (a delay step would click).
        const float to[5] = {delay, fb, c.pre, c.post, c.wet};
        float from[5], d[5];
        glideFrom(to, 5, live && isComb(v.ramp.fType[f]), from);
        const float inv = 1.0f / static_cast<float>(n);
        for (int j = 0; j < 5; ++j) d[j] = (to[j] - from[j]) * inv;
        const bool drives = from[4] > 0.0f || to[4] > 0.0f;
        for (int ch = 0; ch < 2; ++ch) {
            float* x = ch ? R : L;
            float* line = v.comb + (static_cast<size_t>(f) * 2 + static_cast<size_t>(ch)) * kCombLen;
            int w = v.combPos;
            float dl = from[0], g = from[1], pre = from[2], post = from[3], wet = from[4];
            for (int i = 0; i < n; ++i) {
                dl += d[0];
                g += d[1];
                float in = x[i];
                if (drives) {
                    pre += d[2];
                    post += d[3];
                    wet += d[4];
                    in += wet * (softclip(in * pre) * post - in);
                }
                const int d0 = static_cast<int>(dl);
                const float frac = dl - static_cast<float>(d0);
                float a = line[(w - d0) & kMask], b = line[(w - d0 - 1) & kMask];
                if (fill < kCombLen) {   // offsets d0 and d0 + 1 back: this note's only if written since it began
                    const int written = fill + i;
                    if (d0 > written) a = 0.0f;
                    if (d0 + 1 > written) b = 0.0f;
                }
                float y = in + g * (a + frac * (b - a));
                if (dirty) y = softclip(y);
                line[w] = y;
                w = (w + 1) & kMask;
                x[i] = y * (1.0f - 0.55f * std::fabs(g));
            }
        }
        v.combFill[f] = std::min(fill + n, kCombLen);
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
        constexpr int kC = 15;   // per formant k, a1, a2, a3 | drive pre, post, wet
        float to[kC], from[kC];
        for (int j = 0; j < 3; ++j) {
            const float fhz = kVowels[i0][j] + u * (kVowels[i0 + 1][j] - kVowels[i0][j]);
            const SvfCoef fc = makeSvf(tanFast(kPi * std::min(fhz, 0.45f * sr_) / sr_), k);
            to[4 * j] = fc.k;
            to[4 * j + 1] = fc.a1;
            to[4 * j + 2] = fc.a2;
            to[4 * j + 3] = fc.a3;
        }
        to[12] = c.pre;
        to[13] = c.post;
        to[14] = c.wet;
        glideFrom(to, kC, v.ramp.fType[f] == F_VOWEL, from);
        const int steps = (n + kSubChunk - 1) / kSubChunk;
        for (int st = 0; st < steps; ++st) {
            const float s = static_cast<float>(st + 1) / static_cast<float>(steps);
            float cc[kC];
            for (int j = 0; j < kC; ++j) cc[j] = from[j] + (to[j] - from[j]) * s;
            SvfCoef fc[3];
            for (int j = 0; j < 3; ++j) fc[j] = SvfCoef{0.0f, cc[4 * j], cc[4 * j + 1], cc[4 * j + 2], cc[4 * j + 3]};
            const float pre = cc[12], post = cc[13], wet = cc[14];
            const int i1 = st * kSubChunk, i2 = std::min(n, i1 + kSubChunk);
            for (int ch = 0; ch < 2; ++ch) {
                float* x = ch ? R : L;
                for (int i = i1; i < i2; ++i) {
                    const float in = wet > 0.0f ? x[i] + wet * (softclip(x[i] * pre) * post - x[i]) : x[i];
                    float y = 0.0f, b, l;
                    for (int j = 0; j < 3; ++j) {
                        svf(v.svf[f][ch][j], fc[j], in, b, l);
                        y += kFormantGain[j] * fc[j].k * b;
                    }
                    x[i] = 1.6f * y;
                }
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
        for (int k = 0; k < nLanes_; ++k) lanes_[k].v->combFill[f] = 0;
    if (p.type == F_OFF) {
        for (int k = 0; k < nLanes_; ++k) lanes_[k].v->ramp.fType[f] = F_OFF;
        return;
    }
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
    // The state-variable types. Each lane's g (the tuned cutoff), k (resonance) and drive glide
    // from the last chunk's values to this one's in kSubChunk steps: the coefficients are rebuilt
    // per step, so the SVF stays stable all the way (any SVF type ramps into any other).
    constexpr int kC = 5;   // g, k, drive pre, post, wet
    constexpr float kFlat = 1.4142f;   // the 24 dB types' second stage: Q 0.7, no extra peak
    const float piOverSr = kPi / sr_;
    auto isSvf = [](int t) { return t >= F_LP12 && t <= F_PEAK; };
    // A lane's start and end values; the end is the next chunk's start.
    auto ramp = [&](const Lane& l, float* from, float* to) {
        const Ctl ctl = controls(f, l.pitch, l.m, l.mod);
        to[0] = tanFast(piOverSr * ctl.hz);
        to[1] = 1.4142f - 1.36f * ctl.res;   // Q 0.7 .. ~18
        to[2] = ctl.pre;
        to[3] = ctl.post;
        to[4] = ctl.wet;
        Voice& v = *l.v;
        float* last = v.ramp.coef[f];
        const bool glide = v.ramp.valid && isSvf(v.ramp.fType[f]);
        for (int j = 0; j < kC; ++j) {
            from[j] = glide ? last[j] : to[j];
            last[j] = to[j];
        }
        v.ramp.fType[f] = p.type;
    };
    const bool dirty = patch_.engine == EN_DIRTY;
    const int steps = (n + kSubChunk - 1) / kSubChunk;
    const float invSteps = 1.0f / static_cast<float>(steps);
    for (int q = 0; q < nLanes_; q += 4) {
        if (nLanes_ - q == 1) {   // one voice left: scalar
            const Lane& l = lanes_[q];
            float from[kC], to[kC], d[kC];
            ramp(l, from, to);
            for (int j = 0; j < kC; ++j) d[j] = (to[j] - from[j]) * invSteps;
            const bool drive = from[4] > 0.0f || to[4] > 0.0f;
            Svf(&st)[2][3] = l.v->svf[f];
            for (int ch = 0; ch < 2; ++ch) {
                float x[kChunk];
                float* col = (ch ? busR : busL) + q;
                for (int i = 0; i < n; ++i) x[i] = col[i * kMaxVoices];
                float c[kC];
                std::copy(from, from + kC, c);
                for (int sc = 0; sc < steps; ++sc) {
                    for (int j = 0; j < kC; ++j) c[j] += d[j];
                    const SvfCoef s1 = makeSvf(c[0], c[1]), s2 = makeSvf(c[0], kFlat);
                    float* xs = x + sc * kSubChunk;
                    const int len = std::min(kSubChunk, n - sc * kSubChunk);
                    if (drive)
                        for (int i = 0; i < len; ++i) xs[i] += c[4] * (softclip(xs[i] * c[2]) * c[3] - xs[i]);
                    if (dirty) svfScalar<true>(p.type, st[ch][0], st[ch][1], s1, s2, xs, len);
                    else svfScalar<false>(p.type, st[ch][0], st[ch][1], s1, s2, xs, len);
                }
                for (int i = 0; i < n; ++i) col[i * kMaxVoices] = x[i];
            }
            break;
        }
        // Lane j: its voice's values and state; past the last voice a lane that passes silence
        // (g = 0: a1 = 1, a2 = a3 = 0) and is never written back.
        alignas(16) float from[kC][4], to[kC][4];
        alignas(16) float s[2][4][4];        // [channel][stage 1 ic1, ic2, stage 2 ic1, ic2][lane]
        bool drive = false;
        for (int j = 0; j < 4; ++j) {
            const int lane = q + j;
            float cf[kC] = {0.0f, kFlat, 1.0f, 1.0f, 0.0f}, ct[kC] = {0.0f, kFlat, 1.0f, 1.0f, 0.0f};
            if (lane < nLanes_) {
                const Lane& l = lanes_[lane];
                ramp(l, cf, ct);
                drive = drive || cf[4] > 0.0f || ct[4] > 0.0f;
                const Svf(&vs)[2][3] = l.v->svf[f];
                for (int ch = 0; ch < 2; ++ch) {
                    s[ch][0][j] = vs[ch][0].ic1;
                    s[ch][1][j] = vs[ch][0].ic2;
                    s[ch][2][j] = vs[ch][1].ic1;
                    s[ch][3][j] = vs[ch][1].ic2;
                }
            } else {
                for (int ch = 0; ch < 2; ++ch)
                    for (int x = 0; x < 4; ++x) s[ch][x][j] = 0.0f;
            }
            for (int x = 0; x < kC; ++x) {
                from[x][j] = cf[x];
                to[x][j] = ct[x];
            }
        }
        const QuadFn run = quadFor(p.type, dirty);
        f4 st[2][4];
        for (int ch = 0; ch < 2; ++ch)
            for (int x = 0; x < 4; ++x) st[ch][x] = load4(s[ch][x]);
        const f4 inv = splat(invSteps), one = splat(1.0f), flat = splat(kFlat);
        f4 g = load4(from[0]), k = load4(from[1]), pre = load4(from[2]), post = load4(from[3]), wet = load4(from[4]);
        const f4 dg = (load4(to[0]) - g) * inv, dk = (load4(to[1]) - k) * inv, dpre = (load4(to[2]) - pre) * inv,
                 dpost = (load4(to[3]) - post) * inv, dwet = (load4(to[4]) - wet) * inv;
        for (int sc = 0; sc < steps; ++sc) {
            g += dg;
            k += dk;
            SvfV cv, fv;
            cv.k = k;
            cv.a1 = recip4(one + g * (g + k));
            cv.a2 = g * cv.a1;
            cv.a3 = g * cv.a2;
            fv.k = flat;
            fv.a1 = recip4(one + g * (g + flat));
            fv.a2 = g * fv.a1;
            fv.a3 = g * fv.a2;
            DriveV dv;
            if (drive) {
                pre += dpre;
                post += dpost;
                wet += dwet;
                dv = DriveV{pre, post, wet};
            }
            const int len = std::min(kSubChunk, n - sc * kSubChunk), row = sc * kSubChunk * kMaxVoices;
            for (int ch = 0; ch < 2; ++ch) {
                float* bus = (ch ? busR : busL) + q + row;
                if (drive) driveQuad(bus, len, dv);
                run(bus, len, cv, fv, st[ch]);
            }
        }
        for (int ch = 0; ch < 2; ++ch)
            for (int x = 0; x < 4; ++x) store4(s[ch][x], st[ch][x]);
        for (int j = 0; j < 4 && q + j < nLanes_; ++j) {
            Svf(&vs)[2][3] = lanes_[q + j].v->svf[f];
            for (int ch = 0; ch < 2; ++ch) {
                vs[ch][0].ic1 = s[ch][0][j];
                vs[ch][0].ic2 = s[ch][1][j];
                vs[ch][1].ic1 = s[ch][2][j];
                vs[ch][1].ic2 = s[ch][3][j];
            }
        }
    }
}

} // namespace pf
