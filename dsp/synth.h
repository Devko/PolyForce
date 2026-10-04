#pragma once
// The PolyForce voice engine: 8 voices, each 2 wavetable oscillators (up to 8-voice
// unison, stereo spread) -> 2 multimode filters (serial or parallel) -> amp envelope.
// Envelope 2 modulates filter cutoff and wavetable position.
//
// Real-time rules: no allocation, no locks, no exceptions after construction. Everything
// here runs on MPC's audio worker; the plugin layer feeds it a Patch once per block.
#include "wavetable.h"

#include <cstdint>
#include <vector>

namespace pf {

// Ceilings chosen from the device bench: 8 voices x 8 unison x 2 oscillators = 15.5% of a
// block on the Force. surface/surface.py MAX_VOICES / MAX_UNISON must match.
constexpr int kMaxVoices = 8;
constexpr int kMaxUnison = 8;
constexpr int kChunk = 16;   // control rate: filter coefficients, mod envelope, wave position

enum FilterType : int { F_OFF, F_LP12, F_LP24, F_BP, F_HP12, F_HP24, F_NOTCH, F_PEAK, F_COMB_PLUS, F_COMB_MINUS,
                        F_VOWEL };
constexpr int kNumFilterModes = 11;
// Clean: linear filters, steady pitch. Normal: a slow analog pitch drift per voice. Dirty: more
// drift and saturation inside the filter loop (resonance that growls and compresses).
enum EngineMode : int { EN_CLEAN, EN_NORMAL, EN_DIRTY };
enum VoiceMode : int { VM_POLY, VM_DUO, VM_MONO, VM_LEGATO };
// Who gives up its voice when all are busy (Patch::voices of them).
enum StealMode : int { ST_OLDEST, ST_QUIETEST, ST_KEEP_LOW, ST_KEEP_HIGH };
enum GlideMode : int { GL_OFF, GL_ALWAYS, GL_LEGATO };

// What an oscillator plays: the chosen wavetable, or a classic shape (Pulse: position = width).
enum OscWave : int { OW_TABLE, OW_SINE, OW_TRIANGLE, OW_SAW, OW_SQUARE, OW_PULSE, OW_NOISE };
// Where a new note's oscillators start: the phase knob (unison spread around it), anywhere
// (random), or wherever the voice's oscillators happen to be (free running).
enum PhaseMode : int { PH_RESET, PH_RANDOM, PH_FREE };
// Which filter a source feeds. Both: into F1 and F2 alike. Direct: past both filters.
enum Route : int { RT_F1, RT_F2, RT_BOTH, RT_DIRECT };

struct OscPatch {
    int   wave = OW_TABLE;
    const Wavetable* table = nullptr;   // the wavetable (must outlive its use); null = built-in Classic
    float pos = 0.66f;     // wavetable position 0..1; Pulse: width 50% -> 3%; Noise: colour
    float pitch = 0.0f;    // semitones: octave * 12 + semi + fine / 100
    int   unison = 1;      // 1..kMaxUnison
    float detune = 0.3f;   // 0..1 (spread = 100 * detune^2 cents, outermost voices)
    float width = 0.5f;    // stereo spread of the unison voices, 0..1
    float level = 0.8f;    // 0..1
    float pan = 0.0f;      // -1..1
    float phase = 0.0f;    // 0..1 of a cycle (Reset)
    int   phaseMode = PH_RESET;
    int   route = RT_F1;
    int   subWave = CW_SINE;   // Sine, Triangle, Saw, Square (follows the oscillator's pitch)
    float subTune = -12.0f;    // semitones from the oscillator
    float subLevel = 0.0f;     // 0..1
};

struct NoisePatch {
    float level = 0.0f;    // 0..1
    float color = 0.0f;    // -1 dark .. 0 white .. +1 bright
    int   route = RT_F1;
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
    int   voices = kMaxVoices;   // polyphony limit (Poly; Duo uses 2, Mono/Legato 1)
    int   voiceMode = VM_POLY;
    int   steal = ST_OLDEST;
    bool  sameNoteNew = false;   // the same note again: false = retrigger its voice, true = a new voice
    int   glideMode = GL_OFF;    // Always, or Legato = only while another key is held
    bool  glideRate = false;     // false: every glide takes glideTime; true: glideTime per octave
    float glideTime = 0.1f;      // seconds
    float bendUp = 2.0f, bendDown = 2.0f;   // semitones at full pitch bend
    float velCurve = 0.0f;       // -1 (hard) .. 0 (linear) .. +1 (soft): velocity^(4^-curve)
    int   engine = EN_NORMAL;
    bool  parallel = false;      // false: F1's output feeds F2; true: F1 and F2 side by side
    OscPatch osc[2];
    NoisePatch noise;
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
    void pitchBend(float amount);            // -1..1 (the bend ranges are in the Patch)
    void sustain(bool down);
    void allNotesOff();                      // release every voice (CC 123)
    void reset();                            // silence now: CC 120, suspend, transport stop

    void render(float* outL, float* outR, int n);   // overwrites n samples
    int  activeVoices() const;

    // What a voice is doing (tests, diagnostics).
    struct VoiceInfo {
        bool  active, gate;
        int   note;        // the note it plays (or will play after a steal fade)
        float pitch;       // sounding pitch in semitones, gliding toward `note`
        float level;       // amp envelope
    };
    VoiceInfo voiceInfo(int i) const;

    static constexpr int kHeldMax = 16;   // keys remembered for Mono/Legato/Duo note priority
    static constexpr int kFadeSamples = 132;   // ~3 ms: a stolen voice fades before it restarts

private:
    struct Voice {
        bool     active = false;
        bool     gate = false;        // key held
        bool     sustained = false;   // key released while the pedal is down
        int      note = 60;
        float    vel = 1.0f;          // 0..1 after the velocity curve
        float    velGain = 1.0f;
        uint32_t age = 0;             // start order, for stealing the oldest
        float    pitch = 60.0f;       // sounding pitch (semitones), glides toward `note`
        float    glideStep = 0.0f;    // semitones per sample while gliding, 0 = arrived
        int      pendingNote = -1;    // a stolen voice: the note it starts after its fade
        int      pendingVel = 0;
        int      fade = 0;            // samples of fade-out left (stealing)
        Env      env[2];
        uint32_t phase[2][kMaxUnison] = {};
        uint32_t subPhase[2] = {};
        uint32_t noiseRng = 1;
        float    noiseLp[2] = {};          // noise colour filter state, per channel
        float    oscNoiseLp[2][2] = {};    // an oscillator set to Noise: its colour filter, [osc][ch]
        Svf      svf[2][2][3];        // [filter][channel][stage] (Vowel: 3 formants)
        float*   comb = nullptr;      // [filter][channel][kCombLen] delay lines (Synth-owned)
        int      combPos = 0;
        float    drift = 0.0f;        // cents, a slow random walk (Normal / Dirty)
    };
    static constexpr int kCombLen = 4096;   // comb delay: down to 44100 / 4096 = 10.8 Hz
    struct Held { int note; int vel; };

    // Per-oscillator values shared by all voices, rebuilt only when their inputs change.
    struct OscState {
        const Wavetable* table = nullptr;   // what it plays (null: noise)
        int   n = 1;
        float ratio[kMaxUnison] = {};   // detune frequency ratio per unison voice
        float spread[kMaxUnison] = {};  // -1..1 position of each unison voice in the stack
        float gl[kMaxUnison] = {}, gr[kMaxUnison] = {};   // pan * level * 1/sqrt(n)
        float maxRatio = 1.0f;
        int   keyUnison = -1;
        float keyDetune = -1, keyWidth = -1, keyLevel = -1, keyPan = -2;
    };
    // Per-voice modulation for one chunk, in the units the render code uses.
    struct Mods {
        float pitch[2] = {};      // semitones per oscillator
        float pos[2] = {};        // added to the position
        float level[2] = {1, 1};  // oscillator level factor
        float cutoff[2] = {};     // semitones per filter
    };

    void updateOsc(int o, const OscPatch& p);
    static EnvCoef envCoef(const EnvPatch& e, float sr);
    void start(Voice& v, int note, int velocity, bool retrigger = true);
    void startOrSteal(Voice& v, int note, int velocity);
    void release(Voice& v);
    Voice* victim(int limit);
    void glideTo(Voice& v, int note, bool legatoMove);
    float shapeVelocity(int velocity) const;
    void noteOnPoly(int note, int velocity, int limit);
    void noteOnMono(int note, int velocity);
    void holdKey(int note, int velocity);
    void dropKey(int note);
    int  voiceLimit() const;
    void renderVoice(Voice& v, float* outL, float* outR, int n);
    void renderOsc(Voice& v, int o, float pitch, const Mods& m, float* L, float* R, int n) const;
    void renderSub(Voice& v, int o, float pitch, float* L, float* R, int n) const;
    void renderNoise(uint32_t& rng, float* lp, float color, float gain, float* L, float* R, int n) const;
    void filter(Voice& v, int f, float pitch, const Mods& m, float mod, float* L, float* R, int n) const;
    void resetPhases(Voice& v);
    uint32_t random();

    float    sr_;
    Patch    patch_;
    OscState osc_[2];
    EnvCoef  envc_[2];
    Voice    voices_[kMaxVoices];
    float    bend_ = 0.0f;            // -1..1
    bool     pedal_ = false;
    Held     held_[kHeldMax] = {};    // keys down, oldest first
    int      nHeld_ = 0;
    float    lastPitch_ = -1.0f;      // the last note started: where a poly glide comes from
    uint32_t clock_ = 0;
    uint32_t rng_ = 0x9e3779b9u;
    float    cutSemi_[2] = {};       // smoothed cutoff (MIDI-note scale), per filter
    float    vol_ = 0.0f;            // smoothed master gain
    float    volTarget_ = 0.0f;
    bool     fresh_ = true;          // first setPatch: snap smoothers instead of gliding
    std::vector<float> combMem_;     // every voice's comb delay lines, allocated once
};

} // namespace pf
