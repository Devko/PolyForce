#pragma once
// The modulation vocabulary: LFO shapes and sync values, the matrix's sources, targets and
// modifiers. surface/surface.py lists the same names in the same order (patch_map.cpp
// static_asserts the counts).
namespace pf {

enum LfoWave : int { LW_SINE, LW_TRIANGLE, LW_SAW_UP, LW_SAW_DOWN, LW_SQUARE, LW_SAMPLE_HOLD, LW_SMOOTH, LW_COUNT };
// Retrig: per voice, restarts at its phase on every note. Free: per voice, never restarted.
// Global: one LFO for all voices (synced: locked to MPC's bar position).
enum LfoTrig : int { LT_RETRIG, LT_FREE, LT_GLOBAL };

// Tempo-synced LFO and sequencer lengths, in beats (quarter notes). Double: step k sits at
// k * length, and a float triplet length drifts off the host's grid over a song.
constexpr int kNumSyncDivs = 17;
constexpr double kSyncBeats[kNumSyncDivs] = {32.0, 16.0, 8.0, 4.0, 2.0, 4.0 / 3.0, 1.0, 2.0 / 3.0, 1.5,
                                             0.5, 1.0 / 3.0, 0.75, 0.25, 1.0 / 6.0, 0.375, 0.125, 1.0 / 12.0};

enum ModSource : int {
    MS_NONE, MS_ENV1, MS_ENV2, MS_LFO1, MS_LFO2, MS_VELOCITY, MS_NOTE, MS_MODWHEEL, MS_AFTERTOUCH, MS_BEND,
    MS_RANDOM, MS_ALTERNATE, MS_GATE, MS_SEQ, MS_SHAPE1, MS_SHAPE2, MS_SHAPE3, MS_SHAPE4,
    MS_X1, MS_Y1, MS_X2, MS_Y2, MS_X3, MS_Y3, MS_X4, MS_Y4, MS_BREATH, MS_EXPRESSION, MS_CONSTANT,
    MS_COUNT
};

enum ModTarget : int {
    MT_OFF, MT_PITCH, MT_O1_PITCH, MT_O2_PITCH, MT_O1_POS, MT_O2_POS, MT_O1_LEVEL, MT_O2_LEVEL, MT_O1_PAN,
    MT_O2_PAN, MT_O1_DETUNE, MT_O2_DETUNE, MT_SUB1_LEVEL, MT_SUB2_LEVEL, MT_NOISE_LEVEL, MT_NOISE_COLOR,
    MT_F1_CUT, MT_F2_CUT, MT_CUT, MT_F1_RES, MT_F2_RES, MT_F1_DRIVE, MT_F2_DRIVE,
    MT_E1_A, MT_E1_D, MT_E1_S, MT_E1_R, MT_E2_A, MT_E2_D, MT_E2_S, MT_E2_R,
    MT_L1_RATE, MT_L2_RATE, MT_L1_DEPTH, MT_L2_DEPTH, MT_VOLUME, MT_PAN,
    MT_COUNT
};

// Shaping between source and target. Their amount (-1..1) means: Curve: bend toward
// exponential (+) or logarithmic (-); Rectify: none (full rectify); Quantize: 2..16 steps;
// S&H: sample rate 0.5..64 Hz; Slew: 1 ms..2 s.
enum ModModifier : int { MM_NONE, MM_CURVE, MM_RECTIFY, MM_QUANTIZE, MM_SAMPLE_HOLD, MM_SLEW, MM_COUNT };

constexpr int kModSlots = 12;
constexpr int kXyAxes = 8;   // 4 XY pads, X and Y each

// What one unit of a slot's amount does to a target (the amount curve for pitch is a*|a|:
// fine vibrato near 0, wide sweeps at the ends).
enum class Unit : unsigned char { None, Semis, Pos, Level, Pan, Detune, Color, Cutoff, Res, Drive, EnvTime,
                                  EnvLevel, LfoRate, Depth, Volume };
constexpr Unit targetUnit(int t) {
    return t == MT_PITCH || t == MT_O1_PITCH || t == MT_O2_PITCH ? Unit::Semis
         : t == MT_O1_POS || t == MT_O2_POS ? Unit::Pos
         : t == MT_O1_LEVEL || t == MT_O2_LEVEL || t == MT_SUB1_LEVEL || t == MT_SUB2_LEVEL || t == MT_NOISE_LEVEL ? Unit::Level
         : t == MT_O1_PAN || t == MT_O2_PAN || t == MT_PAN ? Unit::Pan
         : t == MT_O1_DETUNE || t == MT_O2_DETUNE ? Unit::Detune
         : t == MT_NOISE_COLOR ? Unit::Color
         : t == MT_F1_CUT || t == MT_F2_CUT || t == MT_CUT ? Unit::Cutoff
         : t == MT_F1_RES || t == MT_F2_RES ? Unit::Res
         : t == MT_F1_DRIVE || t == MT_F2_DRIVE ? Unit::Drive
         : t == MT_E1_A || t == MT_E1_D || t == MT_E1_R || t == MT_E2_A || t == MT_E2_D || t == MT_E2_R ? Unit::EnvTime
         : t == MT_E1_S || t == MT_E2_S ? Unit::EnvLevel
         : t == MT_L1_RATE || t == MT_L2_RATE ? Unit::LfoRate
         : t == MT_L1_DEPTH || t == MT_L2_DEPTH ? Unit::Depth
         : t == MT_VOLUME ? Unit::Volume
         : Unit::None;
}
constexpr float kPitchRange = 48.0f;    // semitones at amount 1 (squared curve)
constexpr float kCutoffRange = 96.0f;   // semitones at amount 1 (8 octaves)
constexpr float kEnvOctavesMod = 4.0f;  // envelope times x/16 .. x16
constexpr float kLfoOctavesMod = 5.0f;  // LFO rate x/32 .. x32

} // namespace pf
