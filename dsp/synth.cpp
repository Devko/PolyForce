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

// Dirty: the same step with the band (resonant) state saturated inside the loop, so loud
// resonance compresses and growls instead of ringing clean.
inline void svfSat(Svf& s, const SvfCoef& c, float v0, float& v1, float& v2) {
    const float v3 = v0 - s.ic2;
    v1 = c.a1 * s.ic1 + c.a2 * v3;
    v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
    s.ic1 = softclip(2.0f * v1 - s.ic1);
    s.ic2 = 2.0f * v2 - s.ic2;
}

template <bool Dirty> inline void step(Svf& s, const SvfCoef& c, float v0, float& v1, float& v2) {
    if (Dirty) svfSat(s, c, v0, v1, v2);
    else svf(s, c, v0, v1, v2);
}

template <bool Dirty> inline float lowpass(Svf& s, const SvfCoef& c, float x) {
    float b, l;
    step<Dirty>(s, c, x, b, l);
    return l;
}

template <bool Dirty> inline float highpass(Svf& s, const SvfCoef& c, float x) {
    float b, l;
    step<Dirty>(s, c, x, b, l);
    return x - c.k * b - l;
}

// The state-variable filter types over one chunk of one channel.
template <bool Dirty>
void svfBlock(int type, Svf& s1, Svf& s2, const SvfCoef& c, const SvfCoef& flat, float* x, int n) {
    float b, l;
    switch (type) {
        case F_LP12: for (int i = 0; i < n; ++i) x[i] = lowpass<Dirty>(s1, c, x[i]); break;
        case F_LP24: for (int i = 0; i < n; ++i) x[i] = lowpass<false>(s2, flat, lowpass<Dirty>(s1, c, x[i])); break;
        case F_HP12: for (int i = 0; i < n; ++i) x[i] = highpass<Dirty>(s1, c, x[i]); break;
        case F_HP24: for (int i = 0; i < n; ++i) x[i] = highpass<false>(s2, flat, highpass<Dirty>(s1, c, x[i])); break;
        case F_BP:   // scaled by k: unity gain at the centre whatever the resonance
            for (int i = 0; i < n; ++i) { step<Dirty>(s1, c, x[i], b, l); x[i] = c.k * b; }
            break;
        case F_NOTCH:
            for (int i = 0; i < n; ++i) { step<Dirty>(s1, c, x[i], b, l); x[i] -= c.k * b; }
            break;
        case F_PEAK:   // low - high
            for (int i = 0; i < n; ++i) { step<Dirty>(s1, c, x[i], b, l); x[i] = 2.0f * l - x[i] + c.k * b; }
            break;
        default: break;
    }
}

} // namespace

Synth::Synth(float sampleRate) : sr_(sampleRate) {
    builtinTables();   // build the shared tables now (UI thread), never on the audio thread
    classicTable(0);
    combMem_.assign(static_cast<size_t>(kMaxVoices) * 2 * 2 * kCombLen, 0.0f);
    for (int i = 0; i < kMaxVoices; ++i) voices_[i].comb = &combMem_[static_cast<size_t>(i) * 2 * 2 * kCombLen];
    setPatch(Patch{});
    fresh_ = true;     // the first real patch snaps its smoothers (no sweep from the defaults)
}

void Synth::setPatch(const Patch& p) {
    patch_ = p;
    patch_.voices = std::clamp(p.voices, 1, kMaxVoices);
    for (int o = 0; o < 2; ++o) updateOsc(o, patch_.osc[o]);
    for (int e = 0; e < 2; ++e) envc_[e] = envCoef(patch_.env[e], sr_);
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
    if (n == s.keyUnison && p.detune == s.keyDetune && p.width == s.keyWidth && p.level == s.keyLevel &&
        p.pan == s.keyPan)
        return;
    s.keyUnison = n;
    s.keyDetune = p.detune;
    s.keyWidth = p.width;
    s.keyLevel = p.level;
    s.keyPan = p.pan;

    // Unison voice u sits at d in [-1, 1]: detuned by d * spread and panned by d * width,
    // lowest voice left, highest right, the whole stack shifted by the pan. 1/sqrt(n) keeps
    // the loudness roughly constant.
    const float cents = 100.0f * p.detune * p.detune;
    const float gain = p.level / std::sqrt(static_cast<float>(n));
    for (int u = 0; u < n; ++u) {
        const float d = n > 1 ? 2.0f * static_cast<float>(u) / static_cast<float>(n - 1) - 1.0f : 0.0f;
        s.spread[u] = d;
        s.ratio[u] = std::exp2(d * cents / 1200.0f);
        const float place = clampf(clampf(p.width, 0.0f, 1.0f) * d + clampf(p.pan, -1.0f, 1.0f), -1.0f, 1.0f);
        const float angle = (place + 1.0f) * kPi * 0.25f;   // equal-power pan
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
    for (int i = 0; !v && i < limit; ++i)
        if (!voices_[i].active) v = &voices_[i];
    if (v) start(*v, note, velocity);
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
    start(v, note, velocity);
}

// All `limit` voices are sounding and a new note needs one of them.
Synth::Voice* Synth::victim(int limit) {
    limit = std::clamp(limit, 1, kMaxVoices);
    auto older = [](const Voice& a, const Voice& b) { return static_cast<int32_t>(a.age - b.age) < 0; };
    Voice* best = nullptr;
    switch (patch_.steal) {
        case ST_QUIETEST: {
            // Released voices first (they are on their way out anyway), quietest first; else
            // the quietest held one.
            for (int pass = 0; pass < 2 && !best; ++pass)
                for (int i = 0; i < limit; ++i) {
                    Voice& x = voices_[i];
                    const bool released = !x.gate && !x.sustained;
                    if (pass == 0 && !released) continue;
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
                if (&voices_[i] != keep && (!best || older(voices_[i], *best))) best = &voices_[i];
            break;
        }
        default:
            for (int i = 0; i < limit; ++i)
                if (!best || older(voices_[i], *best)) best = &voices_[i];
            break;
    }
    return best ? best : &voices_[0];
}

// A stolen voice fades out for ~3 ms, then starts the new note from silence: no click.
void Synth::startOrSteal(Voice& v, int note, int velocity) {
    if (!v.active || v.env[0].v < 1e-3f) {
        v.active = false;
        start(v, note, velocity);
        return;
    }
    v.pendingNote = note;
    v.pendingVel = velocity;
    if (v.fade <= 0) v.fade = kFadeSamples;
    v.gate = false;
    v.sustained = false;
}

void Synth::glideTo(Voice& v, int note, bool legatoMove) {
    v.note = note;
    const bool glide = patch_.glideMode == GL_ALWAYS || (patch_.glideMode == GL_LEGATO && legatoMove);
    const float dist = std::fabs(static_cast<float>(note) - v.pitch);
    if (!glide || dist < 1e-4f || patch_.glideTime <= 1e-4f) {
        v.pitch = static_cast<float>(note);
        v.glideStep = 0.0f;
    } else {
        const float samples = patch_.glideTime * sr_ * (patch_.glideRate ? dist / 12.0f : 1.0f);
        v.glideStep = dist / std::max(samples, 1.0f);
    }
    lastPitch_ = static_cast<float>(note);
}

void Synth::start(Voice& v, int note, int velocity, bool retrigger) {
    const bool wasActive = v.active;
    // Where the pitch comes from: this voice's own pitch if it was sounding, else the last
    // note played (poly glide). "Legato" glides only while another key is held.
    const float from = wasActive ? v.pitch : lastPitch_;
    v.pitch = from >= 0.0f ? from : static_cast<float>(note);
    glideTo(v, note, nHeld_ > 1);
    v.active = true;
    v.gate = true;
    v.sustained = false;
    v.pendingNote = -1;
    v.fade = 0;
    v.age = ++clock_;
    v.vel = shapeVelocity(velocity);
    v.velGain = 1.0f - patch_.velSens + patch_.velSens * v.vel * v.vel;
    if (retrigger)
        for (auto& e : v.env) e.stage = Attack;   // a retriggered voice attacks from where it is

    if (!wasActive) {
        for (auto& e : v.env) e.v = 0.0f;
        for (auto& f : v.svf)
            for (auto& ch : f)
                for (auto& st : ch) st = Svf{};
        if (patch_.flt[0].type >= F_COMB_PLUS || patch_.flt[1].type >= F_COMB_PLUS)
            std::fill(v.comb, v.comb + 2 * 2 * kCombLen, 0.0f);   // only combs read their past
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
        const double base = clampf(p.phase, 0.0f, 1.0f);
        for (int u = 0; u < kMaxUnison; ++u) {
            if (p.phaseMode == PH_RANDOM) {
                v.phase[o][u] = random();
            } else {
                double ph = base + kGolden * u;
                ph -= std::floor(ph);
                v.phase[o][u] = static_cast<uint32_t>(ph * 4294967296.0);
            }
        }
        v.subPhase[o] = p.phaseMode == PH_RANDOM ? random() : static_cast<uint32_t>(base * 4294967296.0);
    }
    v.noiseRng = random() | 1u;
}

void Synth::noteOff(int note) {
    dropKey(note);
    for (auto& v : voices_)   // a stolen voice waiting for this note: let it just fade out
        if (v.active && v.pendingNote == note) v.pendingNote = -1;

    if (patch_.voiceMode == VM_MONO || patch_.voiceMode == VM_LEGATO) {
        Voice& v = voices_[0];
        if (!(v.active && v.gate && v.note == note)) return;
        if (nHeld_ > 0) {   // back to the newest key still held
            const Held& k = held_[nHeld_ - 1];
            if (patch_.voiceMode == VM_LEGATO) glideTo(v, k.note, true);
            else start(v, k.note, k.vel);
            return;
        }
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
                for (const auto& x : voices_) sounding = sounding || (x.active && x.gate && x.note == held_[k].note);
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
        if (v.active) release(v);
    }
}

void Synth::reset() {
    for (auto& v : voices_) {
        v.active = v.gate = v.sustained = false;
        v.pendingNote = -1;
        v.fade = 0;
        v.env[0] = v.env[1] = Env{};
    }
    nHeld_ = 0;
    pedal_ = false;
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
    if (v.glideStep > 0.0f) {   // glide toward the note
        const float target = static_cast<float>(v.note), step = v.glideStep * static_cast<float>(n);
        if (std::fabs(target - v.pitch) <= step) {
            v.pitch = target;
            v.glideStep = 0.0f;
        } else {
            v.pitch += target > v.pitch ? step : -step;
        }
    }
    float pitch = v.pitch + (bend_ >= 0.0f ? bend_ * patch_.bendUp : bend_ * patch_.bendDown);
    if (patch_.engine != EN_CLEAN) {   // analog drift: a slow random walk, a few cents
        const float range = patch_.engine == EN_DIRTY ? 6.0f : 2.5f;
        const float r = static_cast<float>(static_cast<int32_t>(random())) * (1.0f / 2147483648.0f);
        v.drift = clampf(v.drift * 0.9995f + r * 0.08f * range, -range, range);
        pitch += v.drift * 0.01f;
    }

    Mods m;
    for (int o = 0; o < 2; ++o) m.pos[o] = mod * patch_.env2Pos;

    // Three buses: into filter 1, into filter 2, and past both.
    float b1[2][kChunk] = {}, b2[2][kChunk] = {}, bd[2][kChunk] = {};
    float tl[kChunk], tr[kChunk];
    auto send = [&](int route, const float* l, const float* r) {
        float (*to[2])[kChunk] = {nullptr, nullptr};
        switch (route) {
            case RT_F2: to[0] = b2; break;
            case RT_BOTH: to[0] = b1; to[1] = b2; break;
            case RT_DIRECT: to[0] = bd; break;
            default: to[0] = b1; break;
        }
        for (auto* b : to)
            if (b)
                for (int i = 0; i < n; ++i) {
                    b[0][i] += l[i];
                    b[1][i] += r[i];
                }
    };
    for (int o = 0; o < 2; ++o) {
        const OscPatch& p = patch_.osc[o];
        const bool osc = p.level > 0.0f, sub = p.subLevel > 0.0f;
        if (!osc && !sub) continue;
        std::fill(tl, tl + n, 0.0f);
        std::fill(tr, tr + n, 0.0f);
        if (osc) renderOsc(v, o, pitch, m, tl, tr, n);
        if (sub) renderSub(v, o, pitch, tl, tr, n);
        send(p.route, tl, tr);
    }
    if (patch_.noise.level > 0.0f) {
        std::fill(tl, tl + n, 0.0f);
        std::fill(tr, tr + n, 0.0f);
        renderNoise(v.noiseRng, v.noiseLp, patch_.noise.color, 0.5f * patch_.noise.level, tl, tr, n);
        send(patch_.noise.route, tl, tr);
    }

    filter(v, 0, pitch, m, mod, b1[0], b1[1], n);
    if (!patch_.parallel)   // serial: filter 1 feeds filter 2
        for (int i = 0; i < n; ++i) {
            b2[0][i] += b1[0][i];
            b2[1][i] += b1[1][i];
        }
    filter(v, 1, pitch, m, mod, b2[0], b2[1], n);
    for (int i = 0; i < n; ++i) {
        tl[i] = b2[0][i] + bd[0][i] + (patch_.parallel ? b1[0][i] : 0.0f);
        tr[i] = b2[1][i] + bd[1][i] + (patch_.parallel ? b1[1][i] : 0.0f);
    }

    const float vg = v.velGain;
    if (v.fade > 0) {   // being stolen: fade out, then start the waiting note
        constexpr float kInv = 1.0f / static_cast<float>(kFadeSamples);
        for (int i = 0; i < n; ++i) {
            const float a = tick(v.env[0], envc_[0]) * vg * static_cast<float>(std::max(v.fade - i, 0)) * kInv;
            outL[i] += tl[i] * a;
            outR[i] += tr[i] * a;
        }
        v.fade -= n;
        if (v.fade <= 0) {
            v.fade = 0;
            v.active = false;
            if (v.pendingNote >= 0) start(v, v.pendingNote, v.pendingVel);
        }
        return;
    }
    for (int i = 0; i < n; ++i) {
        const float a = tick(v.env[0], envc_[0]) * vg;
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
        const float level = p.level * m.level[o];
        renderNoise(v.noiseRng, v.oscNoiseLp[o], 2.0f * clampf(p.pos + m.pos[o], 0.0f, 1.0f) - 1.0f, 0.5f * level, L, R, n);
        return;
    }
    const Wavetable& t = *s.table;

    const float pos = clampf(p.pos + m.pos[o], 0.0f, 1.0f);
    const float fpos = pos * static_cast<float>(t.frames - 1);
    const int fa = std::min(static_cast<int>(fpos), std::max(t.frames - 2, 0));
    const int fb = std::min(fa + 1, t.frames - 1);
    const float morph = fpos - static_cast<float>(fa);

    const float inc = std::min(noteHz(pitch + p.pitch + m.pitch[o]) / sr_, 0.45f);   // cycles per sample
    const int mip = mipFor(inc * s.maxRatio);                                       // the stack's highest voice decides
    const float* A = t.get(fa, mip);
    const float* B = t.get(fb, mip);
    const float lv = m.level[o];

    // This level's own length: the top `bits` of the phase index it, the rest interpolate.
    const int shift = 32 - kMipBits[mip];
    const uint32_t mask = (1u << shift) - 1;
    const float kFrac = 1.0f / static_cast<float>(1u << shift);
    for (int u = 0; u < s.n; ++u) {
        uint32_t ph = v.phase[o][u];
        const uint32_t dph = static_cast<uint32_t>(inc * s.ratio[u] * 4294967296.0f);
        const float gl = s.gl[u] * lv, gr = s.gr[u] * lv;
        for (int i = 0; i < n; ++i) {
            const uint32_t idx = ph >> shift;
            const float fr = static_cast<float>(ph & mask) * kFrac;
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

// The sub oscillator: one classic-shape voice under the oscillator, panned with it.
void Synth::renderSub(Voice& v, int o, float pitch, float* L, float* R, int n) const {
    const OscPatch& p = patch_.osc[o];
    const Wavetable& t = classicTable(std::clamp(p.subWave, 0, static_cast<int>(CW_SQUARE)));
    const float inc = std::min(noteHz(pitch + p.pitch + p.subTune) / sr_, 0.45f);
    const int mip = mipFor(inc);
    const float* A = t.get(0, mip);
    const int shift = 32 - kMipBits[mip];
    const uint32_t mask = (1u << shift) - 1;
    const float kFrac = 1.0f / static_cast<float>(1u << shift);
    const float angle = (clampf(p.pan, -1.0f, 1.0f) + 1.0f) * kPi * 0.25f;
    const float gl = std::cos(angle) * 1.41421356f * p.subLevel, gr = std::sin(angle) * 1.41421356f * p.subLevel;
    uint32_t ph = v.subPhase[o];
    const uint32_t dph = static_cast<uint32_t>(inc * 4294967296.0f);
    for (int i = 0; i < n; ++i) {
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
    const float g = gain * std::min(1.0f / std::sqrt(std::max(var, 1e-6f)), 6.0f);
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

void Synth::filter(Voice& v, int f, float pitch, const Mods& m, float mod, float* L, float* R, int n) const {
    const FilterPatch& p = patch_.flt[f];
    if (p.type == F_OFF) return;

    // Coefficients once per chunk: cutoff (smoothed) + env 2 + keytrack + modulation, in semitones.
    const float semi = cutSemi_[f] + p.env * mod * kEnvOctaves * 12.0f + p.key * (pitch - 60.0f) + m.cutoff[f];
    const float hz = clampf(noteHz(semi), 16.0f, 0.45f * sr_);
    const float res = clampf(p.res, 0.0f, 1.0f);
    const float drive = clampf(p.drive, 0.0f, 1.0f);
    const float pre = 1.0f + 15.0f * drive * drive;
    const float post = 1.0f / softclip(pre);   // small signals keep ~unity gain
    const bool dirty = patch_.engine == EN_DIRTY;

    if (p.type == F_COMB_PLUS || p.type == F_COMB_MINUS) {
        // A feedback comb tuned to the cutoff (keytrack 100% = the note's pitch): metallic
        // resonances, plucked-string tones. Comb- (inverted feedback) sounds an octave lower
        // and hollow. Resonance = feedback.
        const float delay = clampf(sr_ / hz, 2.0f, static_cast<float>(kCombLen - 2));
        const float fb = (0.25f + 0.72f * res) * (p.type == F_COMB_MINUS ? -1.0f : 1.0f);
        const float out = 1.0f - 0.55f * std::fabs(fb);
        const int d0 = static_cast<int>(delay);
        const float frac = delay - static_cast<float>(d0);
        constexpr int kMask = kCombLen - 1;
        for (int ch = 0; ch < 2; ++ch) {
            float* x = ch ? R : L;
            float* line = v.comb + (static_cast<size_t>(f) * 2 + static_cast<size_t>(ch)) * kCombLen;
            int w = v.combPos;
            for (int i = 0; i < n; ++i) {
                const float in = drive > 0.0f ? softclip(x[i] * pre) * post : x[i];
                const float a = line[(w - d0) & kMask], b = line[(w - d0 - 1) & kMask];
                float y = in + fb * (a + frac * (b - a));
                if (dirty) y = softclip(y);
                line[w] = y;
                w = (w + 1) & kMask;
                x[i] = y * out;
            }
        }
        if (f == 1 || patch_.flt[1].type < F_COMB_PLUS)   // both filters share the write position
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
            fc[j] = makeSvf(std::tan(kPi * std::min(fhz, 0.45f * sr_) / sr_), k);
        }
        for (int ch = 0; ch < 2; ++ch) {
            float* x = ch ? R : L;
            for (int i = 0; i < n; ++i) {
                const float in = drive > 0.0f ? softclip(x[i] * pre) * post : x[i];
                float y = 0.0f, b, l;
                for (int j = 0; j < 3; ++j) {
                    svf(v.svf[f][ch][j], fc[j], in, b, l);
                    y += kFormantGain[j] * k * b;
                }
                x[i] = 1.6f * y;
            }
        }
        return;
    }

    const float g = std::tan(kPi * hz / sr_);
    const SvfCoef c = makeSvf(g, 1.4142f - 1.36f * res);   // Q 0.7 .. ~18
    const SvfCoef flat = makeSvf(g, 1.4142f);   // second stage of the 24 dB types: no extra peak

    for (int ch = 0; ch < 2; ++ch) {
        float* x = ch ? R : L;
        if (drive > 0.0f)
            for (int i = 0; i < n; ++i) x[i] = softclip(x[i] * pre) * post;
        if (dirty) svfBlock<true>(p.type, v.svf[f][ch][0], v.svf[f][ch][1], c, flat, x, n);
        else svfBlock<false>(p.type, v.svf[f][ch][0], v.svf[f][ch][1], c, flat, x, n);
    }
}

} // namespace pf
