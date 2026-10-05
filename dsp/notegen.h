#pragma once
// Note generation between MIDI in and the Synth: the arpeggiator and the 16-step sequencer
// (which also runs as a modulation source without playing notes), plus the 8-step shape
// sequencer's four outputs.
//
// Timing: one clock in beats. While MPC plays it follows MPC's bar position (steps land on
// the song's grid); stopped, it counts on its own from the first key. The plugin walks each
// block in time order, rendering up to untilNext() samples at a time and calling advance(),
// so generated notes start sample-accurately. Real time: no allocation, no locks.
#include "mod.h"
#include "synth.h"

#include <cstdint>

namespace pf {

enum SeqMode : int { SQ_OFF, SQ_ARP, SQ_SEQ };
enum ArpDir : int { AD_UP, AD_DOWN, AD_UP_DOWN, AD_DOWN_UP, AD_PLAYED, AD_RANDOM, AD_CHORD, AD_COUNT };
enum ShapeMode : int { SM_STEP, SM_RAMP, SM_SMOOTH };

constexpr int kSeqSteps = 16;
constexpr int kShapeSteps = 8;
constexpr int kShapeLanes = 4;

struct SeqPatch {
    int   mode = SQ_OFF;
    int   dir = AD_UP;
    int   octaves = 1;       // 1..4
    int   rate = 12;         // kSyncBeats index: 1/16
    float gate = 0.5f;       // of a step, 0.05..1
    float swing = 0.0f;      // 0..0.5: every second step later by this share of a step
    bool  latch = false;     // the arp keeps playing after the keys go up
    bool  pattern = false;   // the arp follows the step lane (velocity 0 = rest, note = transpose)
    int   steps = 16;        // 1..16
    bool  record = false;    // keys write steps (step-by-step when stopped, at the playing step otherwise)
    int   note[kSeqSteps] = {};     // semitones from the key, -24..24
    int   vel[kSeqSteps];           // 0 = rest, 1..127
    float mod[kSeqSteps] = {};      // the Seq mod source, -1..1
    int   shapeRate = 9;            // 1/8
    int   shapeSteps = 8;
    int   shapeMode[kShapeLanes] = {};
    float shape[kShapeLanes][kShapeSteps] = {};   // -1..1
    SeqPatch() { for (int& v : vel) v = 100; }
};

class NoteGen {
public:
    explicit NoteGen(float sampleRate = 44100.0f);

    void setPatch(const SeqPatch& p, Synth& synth);   // block start (mode changes stop generated notes)
    void setTransport(double bpm, double beats, bool playing, bool beatsValid);   // block start

    // Keys from MPC. With the arp/sequencer off they go straight to the synth.
    void keyOn(int note, int vel, Synth& synth);
    void keyOff(int note, Synth& synth);
    // The sustain pedal: with the arp/sequencer off it sustains the synth's notes; on, it holds
    // the keys (released keys stay in the arp until the pedal goes up), not every generated note.
    void pedal(bool down, Synth& synth);
    void panic(Synth& synth);    // all keys up, generated notes off

    int  untilNext(int limit) const;          // samples to the next generated event, at most limit (>= 1)
    void advance(int samples, Synth& synth);  // moves the clock, fires what is due

    // Mod sources for the block (Seq lane value, the four shape outputs).
    float seqValue() const { return seqValue_; }
    void  shapeValues(float* out4) const;

    // A step the recorder wrote (the plugin turns it into parameter values). -1 = none.
    struct Recorded { int step, note, vel; };
    bool  takeRecorded(Recorded& r);

    bool  running() const { return running_; }
    int   currentStep() const { return lastStep_; }

private:
    struct Off { int note; double at; };   // a generated note's end, in beats
    void startClock();
    void rebase(double beat);               // the clock moves to `beat`: generated notes' ends move with it
    // The next step = the first boundary at or after `beat`; skipFired: not one that fired a
    // moment ago (a loop's end, a regrid right after a step).
    void reaim(double beat, bool skipFired);
    // Step indices are 64-bit: a 32-bit long (ARMv7) overflows at a large host ppq.
    double boundary(int64_t k) const;       // beat of step k (with swing)
    int  wrap(int64_t k) const { const int64_t n = p_.steps; return static_cast<int>(((k % n) + n) % n); }
    void releaseHeard(Synth& synth);
    void fireStep(int64_t k, Synth& synth);
    void stopGenerated(Synth& synth);
    int  arpNotes(int* out, int max) const;   // the arp's note list, in play order
    bool keysDown() const { return nKeys_ > 0 || (p_.latch && nLatched_ > 0); }
    uint32_t random();

    float    sr_;
    SeqPatch p_;
    double   bpm_ = 120.0;
    double   beat_ = 0.0;        // the clock
    bool     playing_ = false;
    bool     running_ = false;   // generating notes
    int64_t  next_ = 0;          // the next step boundary's index
    int64_t  lastFired_ = -1;
    int      lastStep_ = -1;
    int64_t  arpIndex_ = 0;
    float    seqValue_ = 0.0f;

    struct Key { int note, vel; bool pedal; };   // pedal: the key is up, the pedal holds it
    Key  keys_[16] = {};          // down (or held by the pedal), in press order
    int  nKeys_ = 0;
    Key  latched_[16] = {};       // the latched chord (arp latch)
    int  nLatched_ = 0;
    bool latchFresh_ = true;      // the next key down starts a new latched chord
    bool pedal_ = false;
    double lastFireBeat_ = -1e30; // when the last step fired (a loop point must not fire it twice)
    int  heard_[16] = {};         // notes played straight to the synth while recording
    int  nHeard_ = 0;
    Off  offs_[64] = {};          // sounding generated notes
    int  nOffs_ = 0;
    int  recPos_ = 0;
    Recorded rec_[16] = {};
    int  nRec_ = 0;
    uint32_t rng_ = 0x6d2b79f5u;
};

} // namespace pf
