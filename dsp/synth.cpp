#include "synth.h"

#include <algorithm>
#include <cmath>

namespace pf {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kHeadroom = 0.25f;   // -12 dB per voice, so a 4-note chord at full level doesn't clip
constexpr float kEnvOctaves = 8.0f;  // filter env amount +-1 = +-8 octaves

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
inline float noteHz(float note) { return 440.0f * std::exp2((note - 69.0f) / 12.0f); }
inline float hzNote(float hz) { return 69.0f + 12.0f * std::log2(std::max(hz, 1.0f) / 440.0f); }

// tanh-like saturator, exactly +-1 from |x| = 3 on.
inline float softclip(float x) {
    x = clampf(x, -3.0f, 3.0f);
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

// Per-sample one-pole step for a time constant of `tau` seconds.
inline float onePole(float tau, float sr) { return 1.0f - std::exp(-1.0f / (std::max(tau, 1e-5f) * sr)); }

inline float tick(Env& e, const EnvCoef& c) {
    switch (e.stage) {
        case Attack:
            e.v += (1.2f - e.v) * c.att;
            if (e.v >= 1.0f) {
                e.v = 1.0f;
                e.stage = Decay;
            }
            break;
        case Decay:   // decay and sustain are one segment: it settles on the sustain level
            e.v += (c.sus - e.v) * c.dec;
            break;
        case Release:
            e.v -= e.v * c.rel;
            if (e.v < 1e-4f) {   // -80 dB: done
                e.v = 0.0f;
                e.stage = Idle;
            }
            break;
        case Idle:
            break;
    }
    return e.v;
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

inline float lowpass(Svf& s, const SvfCoef& c, float x) {
    float b, l;
    svf(s, c, x, b, l);
    return l;
}

inline float highpass(Svf& s, const SvfCoef& c, float x) {
    float b, l;
    svf(s, c, x, b, l);
    return x - c.k * b - l;
}

} // namespace

Synth::Synth(float sampleRate) : sr_(sampleRate) {
    builtinTables();   // build the shared tables now (UI thread), never on the audio thread
    setPatch(Patch{});
}

void Synth::setPatch(const Patch& p) {
    patch_ = p;
    patch_.voices = std::clamp(p.voices, 1, kMaxVoices);
    for (int o = 0; o < 2; ++o) updateOsc(o, patch_.osc[o]);
    for (int e = 0; e < 2; ++e) envc_[e] = envCoef(patch_.env[e], sr_);
    volTarget_ = patch_.volumeDb <= -59.5f ? 0.0f : std::pow(10.0f, patch_.volumeDb / 20.0f) * kHeadroom;

    // Polyphony lowered while playing: let the voices above the new limit ring out.
    for (int i = patch_.voices; i < kMaxVoices; ++i)
        if (voices_[i].active && (voices_[i].gate || voices_[i].sustained)) release(voices_[i]);

    if (fresh_) {   // first patch: start the smoothers on target instead of gliding from 0
        for (int f = 0; f < 2; ++f) cutSemi_[f] = hzNote(patch_.flt[f].cutoffHz);
        vol_ = volTarget_;
        fresh_ = false;
    }
}

void Synth::updateOsc(int o, const OscPatch& p) {
    OscState& s = osc_[o];
    const auto& tables = builtinTables();
    s.table = p.table && p.table->frames > 0
                  ? p.table
                  : &tables[static_cast<size_t>(std::clamp(p.wave, 0, static_cast<int>(tables.size()) - 1))];

    const int n = std::clamp(p.unison, 1, kMaxUnison);
    if (n == s.keyUnison && p.detune == s.keyDetune && p.width == s.keyWidth && p.level == s.keyLevel) return;
    s.keyUnison = n;
    s.keyDetune = p.detune;
    s.keyWidth = p.width;
    s.keyLevel = p.level;

    // Unison voice u sits at d in [-1, 1]: detuned by d * spread and panned by d * width,
    // lowest voice left, highest right. 1/sqrt(n) keeps the loudness roughly constant.
    const float cents = 100.0f * p.detune * p.detune;
    const float gain = p.level / std::sqrt(static_cast<float>(n));
    for (int u = 0; u < n; ++u) {
        const float d = n > 1 ? 2.0f * static_cast<float>(u) / static_cast<float>(n - 1) - 1.0f : 0.0f;
        s.ratio[u] = std::exp2(d * cents / 1200.0f);
        const float angle = (clampf(p.width, 0.0f, 1.0f) * d + 1.0f) * kPi * 0.25f;   // equal-power pan
        s.gl[u] = std::cos(angle) * 1.41421356f * gain;
        s.gr[u] = std::sin(angle) * 1.41421356f * gain;
    }
    s.n = n;
    s.maxRatio = n > 1 ? std::exp2(cents / 1200.0f) : 1.0f;
}

EnvCoef Synth::envCoef(const EnvPatch& e, float sr) {
    EnvCoef c;
    c.att = onePole(e.a / 1.7918f, sr);   // ln(1.2 / 0.2): aiming at 1.2, the curve crosses 1.0 at `a`
    c.dec = onePole(e.d / 6.9078f, sr);   // ln(1000): 60 dB of the way at `d`
    c.rel = onePole(e.r / 6.9078f, sr);
    c.sus = clampf(e.s, 0.0f, 1.0f);
    return c;
}

// --- notes --------------------------------------------------------------------------------

void Synth::noteOn(int note, int velocity) {
    if (velocity <= 0) {
        noteOff(note);
        return;
    }
    Voice* v = nullptr;
    for (auto& x : voices_)   // the same note again: retrigger its voice, don't stack a second one
        if (x.active && x.note == note) {
            v = &x;
            break;
        }
    for (int i = 0; !v && i < patch_.voices; ++i)
        if (!voices_[i].active) v = &voices_[i];
    if (!v) v = victim();
    start(*v, note, velocity);
}

// All patch_.voices voices are sounding and a new note needs one of them.
Synth::Voice* Synth::victim() {
    // TODO(you): the voice-stealing policy. Placeholder: steal the oldest voice.
    Voice* best = &voices_[0];
    for (int i = 1; i < patch_.voices; ++i)
        if (static_cast<int32_t>(voices_[i].age - best->age) < 0) best = &voices_[i];
    return best;
}

void Synth::start(Voice& v, int note, int velocity) {
    const bool wasActive = v.active;
    v.active = true;
    v.gate = true;
    v.sustained = false;
    v.note = note;
    v.age = ++clock_;
    const float vel = static_cast<float>(std::clamp(velocity, 1, 127)) / 127.0f;
    v.velGain = 1.0f - patch_.velSens + patch_.velSens * vel * vel;
    for (auto& e : v.env) e.stage = Attack;   // a stolen/retriggered voice attacks from where it is

    if (!wasActive) {
        for (auto& e : v.env) e.v = 0.0f;
        for (auto& f : v.svf)
            for (auto& ch : f)
                for (auto& st : ch) st = Svf{};
        // Unison voices start at random phases (a phase-aligned stack flanges); a single
        // voice starts at 0 so its attack is the same every time.
        for (int o = 0; o < 2; ++o)
            for (int u = 0; u < kMaxUnison; ++u) v.phase[o][u] = osc_[o].n > 1 ? random() : 0u;
    }
}

void Synth::noteOff(int note) {
    for (auto& v : voices_)
        if (v.active && v.gate && v.note == note) {
            v.gate = false;
            if (pedal_)
                v.sustained = true;
            else
                release(v);
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

void Synth::pitchBend(float semitones) { bend_ = semitones; }

void Synth::allNotesOff() {
    for (auto& v : voices_)
        if (v.active) release(v);
}

void Synth::reset() {
    for (auto& v : voices_) {
        v.active = v.gate = v.sustained = false;
        v.env[0] = v.env[1] = Env{};
    }
    pedal_ = false;
}

int Synth::activeVoices() const {
    int n = 0;
    for (const auto& v : voices_) n += v.active ? 1 : 0;
    return n;
}

uint32_t Synth::random() {   // xorshift32
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
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
        for (int f = 0; f < 2; ++f) cutSemi_[f] += (cutTarget[f] - cutSemi_[f]) * 0.2f;

        for (auto& v : voices_)
            if (v.active) renderVoice(v, L, R, len);

        const float g0 = vol_;
        vol_ += (volTarget_ - vol_) * 0.07f;
        const float step = (vol_ - g0) / static_cast<float>(len);
        float g = g0;
        for (int i = 0; i < len; ++i) {
            g += step;
            L[i] *= g;
            R[i] *= g;
        }
    }
}

void Synth::renderVoice(Voice& v, float* outL, float* outR, int n) {
    // Control rate: the mod envelope's value at the start of the chunk drives this chunk.
    const float mod = v.env[1].v;
    for (int i = 0; i < n; ++i) tick(v.env[1], envc_[1]);
    const float pitch = static_cast<float>(v.note) + bend_;

    float l[2][kChunk] = {}, r[2][kChunk] = {};
    for (int o = 0; o < 2; ++o)
        if (patch_.osc[o].level > 0.0f) renderOsc(v, o, pitch, mod, l[o], r[o], n);

    if (!patch_.parallel) {   // osc1 + osc2 -> F1 -> F2
        for (int i = 0; i < n; ++i) {
            l[0][i] += l[1][i];
            r[0][i] += r[1][i];
        }
        filter(v, 0, pitch, mod, l[0], r[0], n);
        filter(v, 1, pitch, mod, l[0], r[0], n);
    } else {                  // osc1 -> F1, osc2 -> F2
        filter(v, 0, pitch, mod, l[0], r[0], n);
        filter(v, 1, pitch, mod, l[1], r[1], n);
        for (int i = 0; i < n; ++i) {
            l[0][i] += l[1][i];
            r[0][i] += r[1][i];
        }
    }

    const float vg = v.velGain;
    for (int i = 0; i < n; ++i) {
        const float a = tick(v.env[0], envc_[0]) * vg;
        outL[i] += l[0][i] * a;
        outR[i] += r[0][i] * a;
    }
    if (v.env[0].stage == Idle) v.active = false;
}

// The hot loop. Phase is a 32-bit fixed-point fraction of a cycle: the top 11 bits index
// the 2048-sample frame, the low 21 bits are the interpolation fraction, and wrap-around
// is free integer overflow (no floor(), no branch).
void Synth::renderOsc(Voice& v, int o, float pitch, float mod, float* L, float* R, int n) const {
    const OscState& s = osc_[o];
    const OscPatch& p = patch_.osc[o];
    const Wavetable& t = *s.table;

    const float pos = clampf(p.pos + mod * patch_.env2Pos, 0.0f, 1.0f);
    const float fpos = pos * static_cast<float>(t.frames - 1);
    const int fa = std::min(static_cast<int>(fpos), std::max(t.frames - 2, 0));
    const int fb = std::min(fa + 1, t.frames - 1);
    const float morph = fpos - static_cast<float>(fa);

    const float inc = std::min(noteHz(pitch + p.pitch) / sr_, 0.45f);   // cycles per sample
    const int mip = mipFor(inc * s.maxRatio);                            // the stack's highest voice decides
    const float* A = t.get(fa, mip);
    const float* B = t.get(fb, mip);

    constexpr float kFrac = 1.0f / static_cast<float>(1u << (32 - kTableBits));
    for (int u = 0; u < s.n; ++u) {
        uint32_t ph = v.phase[o][u];
        const uint32_t dph = static_cast<uint32_t>(inc * s.ratio[u] * 4294967296.0f);
        const float gl = s.gl[u], gr = s.gr[u];
        for (int i = 0; i < n; ++i) {
            const uint32_t idx = ph >> (32 - kTableBits);
            const float fr = static_cast<float>(ph & ((1u << (32 - kTableBits)) - 1)) * kFrac;
            const float a = A[idx] + fr * (A[idx + 1] - A[idx]);
            const float b = B[idx] + fr * (B[idx + 1] - B[idx]);
            const float x = a + morph * (b - a);
            L[i] += x * gl;
            R[i] += x * gr;
            ph += dph;
        }
        v.phase[o][u] = ph;
    }
}

void Synth::filter(Voice& v, int f, float pitch, float mod, float* L, float* R, int n) const {
    const FilterPatch& p = patch_.flt[f];
    if (p.type == F_OFF) return;

    // Coefficients once per chunk: cutoff (smoothed) + env 2 + keytrack, all in semitones.
    const float semi = cutSemi_[f] + p.env * mod * kEnvOctaves * 12.0f + p.key * (pitch - 60.0f);
    const float hz = clampf(noteHz(semi), 16.0f, 0.45f * sr_);
    const float g = std::tan(kPi * hz / sr_);
    const SvfCoef c = makeSvf(g, 1.4142f - 1.36f * clampf(p.res, 0.0f, 1.0f));   // Q 0.7 .. ~18
    const SvfCoef flat = makeSvf(g, 1.4142f);   // second stage of the 24 dB types: no extra peak

    const float drive = clampf(p.drive, 0.0f, 1.0f);
    const float pre = 1.0f + 15.0f * drive * drive;
    const float post = 1.0f / softclip(pre);   // small signals keep ~unity gain

    for (int ch = 0; ch < 2; ++ch) {
        float* x = ch ? R : L;
        Svf& s1 = v.svf[f][ch][0];
        Svf& s2 = v.svf[f][ch][1];
        if (drive > 0.0f)
            for (int i = 0; i < n; ++i) x[i] = softclip(x[i] * pre) * post;

        float b, l;
        switch (p.type) {
            case F_LP12: for (int i = 0; i < n; ++i) x[i] = lowpass(s1, c, x[i]); break;
            case F_LP24: for (int i = 0; i < n; ++i) x[i] = lowpass(s2, flat, lowpass(s1, c, x[i])); break;
            case F_HP12: for (int i = 0; i < n; ++i) x[i] = highpass(s1, c, x[i]); break;
            case F_HP24: for (int i = 0; i < n; ++i) x[i] = highpass(s2, flat, highpass(s1, c, x[i])); break;
            case F_BP:   // scaled by k: unity gain at the centre whatever the resonance
                for (int i = 0; i < n; ++i) { svf(s1, c, x[i], b, l); x[i] = c.k * b; }
                break;
            case F_NOTCH:
                for (int i = 0; i < n; ++i) { svf(s1, c, x[i], b, l); x[i] -= c.k * b; }
                break;
            case F_PEAK:   // low - high
                for (int i = 0; i < n; ++i) { svf(s1, c, x[i], b, l); x[i] = 2.0f * l - x[i] + c.k * b; }
                break;
            default: break;
        }
    }
}

} // namespace pf
