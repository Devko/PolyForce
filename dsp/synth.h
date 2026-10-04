#pragma once
// The PolyForce voice engine: 8 voices, each 2 wavetable oscillators (up to 8-voice
// unison, stereo spread) -> 2 multimode filters (serial or parallel) -> amp envelope.
// Envelope 2 modulates filter cutoff and wavetable position.
//
// Real-time rules: no allocation, no locks, no exceptions after construction. Everything
// here runs on MPC's audio worker; the plugin layer feeds it a Patch once per block.
#include "wavetable.h"

#include <cstdint>

namespace pf {

// Ceilings chosen from the device bench: 8 voices x 8 unison x 2 oscillators = 15.5% of a
// block on the Force. surface/surface.py MAX_VOICES / MAX_UNISON must match.
constexpr int kMaxVoices = 8;
constexpr int kMaxUnison = 8;
constexpr int kChunk = 16;   // control rate: filter coefficients, mod envelope, wave position

enum FilterType : int { F_OFF, F_LP12, F_LP24, F_BP, F_HP12, F_HP24, F_NOTCH, F_PEAK };

struct OscPatch {
    int   wave = 0;        // index into builtinTables()
    const Wavetable* table = nullptr;   // an imported table instead (must outlive its use)
    float pos = 0.66f;     // wavetable position 0..1
    float pitch = 0.0f;    // semitones: octave * 12 + semi + fine / 100
    int   unison = 1;      // 1..kMaxUnison
    float detune = 0.3f;   // 0..1 (spread = 100 * detune^2 cents, outermost voices)
    float width = 0.5f;    // stereo spread of the unison voices, 0..1
    float level = 0.8f;    // 0..1
};

struct FilterPatch {
    int   type = F_LP24;
    float cutoffHz = 1200.0f;
    float res = 0.25f;     // 0..1
    float env = 0.25f;     // env 2 amount, -1..1 = -8..+8 octaves
    float key = 0.5f;      // keytrack 0..1 (1 = cutoff follows the note exactly)
    float drive = 0.0f;    // 0..1 pre-filter saturation
};

struct EnvPatch {
    float a = 0.003f, d = 0.4f, s = 0.8f, r = 0.3f;   // seconds, sustain 0..1
};

struct Patch {
    float volumeDb = -6.0f;
    int   voices = kMaxVoices;   // polyphony limit
    bool  parallel = false;      // false: osc1+osc2 -> F1 -> F2; true: osc1 -> F1, osc2 -> F2
    OscPatch osc[2];
    FilterPatch flt[2];
    EnvPatch env[2];
    float velSens = 0.5f;        // env 1 velocity sensitivity 0..1
    float env2Pos = 0.0f;        // env 2 -> wavetable position, -1..1
};

// Envelope: analog-style one-pole segments (attack aims at 1.2 and stops at 1.0).
enum EnvStage : uint8_t { Idle, Attack, Decay, Release };
struct Env { EnvStage stage = Idle; float v = 0.0f; };
struct EnvCoef { float att = 0, dec = 0, rel = 0, sus = 0; };   // per-sample one-pole steps

// Trapezoidal (zero-delay feedback) state-variable filter, A. Simper / Cytomic.
struct Svf { float ic1 = 0.0f, ic2 = 0.0f; };
struct SvfCoef { float g = 0, k = 2, a1 = 0, a2 = 0, a3 = 0; };

class Synth {
public:
    explicit Synth(float sampleRate = 44100.0f);

    void setPatch(const Patch& p);           // between render() calls
    void noteOn(int note, int velocity);     // velocity 0 = note off
    void noteOff(int note);
    void pitchBend(float semitones);
    void sustain(bool down);
    void allNotesOff();                      // release every voice (CC 123)
    void reset();                            // silence now: CC 120, suspend, transport stop

    void render(float* outL, float* outR, int n);   // overwrites n samples
    int  activeVoices() const;

private:
    struct Voice {
        bool     active = false;
        bool     gate = false;        // key held
        bool     sustained = false;   // key released while the pedal is down
        int      note = 60;
        float    velGain = 1.0f;
        uint32_t age = 0;             // start order, for stealing the oldest
        Env      env[2];
        uint32_t phase[2][kMaxUnison] = {};
        Svf      svf[2][2][2];        // [filter][channel][stage]
    };

    // Per-oscillator values shared by all voices, rebuilt only when their inputs change.
    struct OscState {
        const Wavetable* table = nullptr;
        int   n = 1;
        float ratio[kMaxUnison] = {};   // detune frequency ratio per unison voice
        float gl[kMaxUnison] = {}, gr[kMaxUnison] = {};   // pan * level * 1/sqrt(n)
        float maxRatio = 1.0f;
        int   keyUnison = -1;
        float keyDetune = -1, keyWidth = -1, keyLevel = -1;
    };

    void updateOsc(int o, const OscPatch& p);
    static EnvCoef envCoef(const EnvPatch& e, float sr);
    void start(Voice& v, int note, int velocity);
    void release(Voice& v);
    Voice* victim();
    void renderVoice(Voice& v, float* outL, float* outR, int n);
    void renderOsc(Voice& v, int o, float pitch, float mod, float* L, float* R, int n) const;
    void filter(Voice& v, int f, float pitch, float mod, float* L, float* R, int n) const;
    uint32_t random();

    float    sr_;
    Patch    patch_;
    OscState osc_[2];
    EnvCoef  envc_[2];
    Voice    voices_[kMaxVoices];
    float    bend_ = 0.0f;
    bool     pedal_ = false;
    uint32_t clock_ = 0;
    uint32_t rng_ = 0x9e3779b9u;
    float    cutSemi_[2] = {};       // smoothed cutoff (MIDI-note scale), per filter
    float    vol_ = 0.0f;            // smoothed master gain
    float    volTarget_ = 0.0f;
    bool     fresh_ = true;          // first setPatch: snap smoothers instead of gliding
};

} // namespace pf
