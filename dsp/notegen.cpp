#include "notegen.h"

#include <algorithm>
#include <cmath>

namespace pf {
namespace {
constexpr double kEps = 1e-9;
constexpr float kPi = 3.14159265f;
// NaN-safe (std::clamp passes NaN through, and a NaN gate is a note that never ends).
inline float clampf(float v, float lo, float hi) { return v > lo ? (v < hi ? v : hi) : lo; }
}

NoteGen::NoteGen(float sampleRate) : sr_(sampleRate) {}

uint32_t NoteGen::random() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
}

void NoteGen::setPatch(const SeqPatch& in, Synth& synth) {
    SeqPatch p = in;
    p.octaves = std::clamp(p.octaves, 1, 4);
    p.steps = std::clamp(p.steps, 1, kSeqSteps);
    p.shapeSteps = std::clamp(p.shapeSteps, 1, kShapeSteps);
    p.rate = std::clamp(p.rate, 0, kNumSyncDivs - 1);
    p.shapeRate = std::clamp(p.shapeRate, 0, kNumSyncDivs - 1);
    p.gate = clampf(p.gate, 0.05f, 1.0f);
    p.swing = clampf(p.swing, 0.0f, 0.5f);
    for (float& m : p.mod) m = clampf(m, -1.0f, 1.0f);
    for (auto& lane : p.shape)
        for (float& v : lane) v = clampf(v, -1.0f, 1.0f);
    const int before = p_.mode;
    const bool newRate = p.rate != p_.rate, newSwing = p.swing != p_.swing;
    if (p.mode != before || p.record != p_.record) releaseHeard(synth);
    p_ = p;
    if (newRate) {
        reaim(beat_, true);   // the step index means another beat at the new rate
    } else if (newSwing) {    // the same steps, moved: one that already played doesn't play again
        const int64_t fired = lastFired_;
        reaim(beat_, false);
        next_ = std::max(next_, fired + 1);
        lastFired_ = std::max(lastFired_, fired);
    }
    if (p_.mode != before) {
        stopGenerated(synth);
        // The keys were playing straight through: they stop too, all of them (a 17th key
        // pushed the oldest out of keys_ while it kept sounding).
        if (before == SQ_OFF) synth.allNotesOff();
        if (pedal_) synth.sustain(p_.mode == SQ_OFF);   // the pedal holds notes only when off
        if (p_.mode == SQ_OFF) {   // keys only the pedal held for the arp are up: they go
            int w = 0;
            for (int i = 0; i < nKeys_; ++i)
                if (!keys_[i].pedal) keys_[w++] = keys_[i];
            nKeys_ = w;
        }
        arpIndex_ = 0;
    }
    if (!p_.latch) nLatched_ = 0;
    running_ = p_.mode != SQ_OFF && keysDown();
    if (lastStep_ >= p_.steps) lastStep_ = wrap(lastStep_);
    if (recPos_ >= p_.steps) recPos_ = 0;

    // The mod lanes, for this block (a step that fires inside it updates the Seq lane there).
    int idx = -1;
    if (running_ && lastStep_ >= 0) idx = lastStep_;
    else if (playing_ || keysDown()) idx = wrap(static_cast<int64_t>(std::floor(beat_ / kSyncBeats[p_.rate])));
    seqValue_ = idx >= 0 ? p_.mod[idx] : 0.0f;
}

void NoteGen::setTransport(double bpm, double beats, bool playing, bool beatsValid) {
    bpm_ = std::isfinite(bpm) && bpm >= 1.0 && bpm <= 1000.0 ? bpm : 120.0;   // the plugin's own bounds
    if (playing && beatsValid && std::isfinite(beats)) {
        // Started, or jumped (a loop, a locate): re-aim at the first step at or after here.
        // A few ms of difference is the host's rounding, not a jump. Only a jump while playing
        // can be a loop's end (a free-running step that fired just before Play is not one).
        const bool started = !playing_;
        const bool jumped = started || std::fabs(beats - beat_) > 0.01;
        rebase(beats);
        if (jumped) reaim(beats, !started);
        beat_ = beats;
        playing_ = true;
    } else {
        playing_ = false;
    }
}

void NoteGen::rebase(double beat) {
    const double d = beat - beat_;
    for (int i = 0; i < nOffs_; ++i) offs_[i].at += d;
    lastFireBeat_ += d;
}

void NoteGen::reaim(double beat, bool skipFired) {
    const double s = kSyncBeats[p_.rate];
    int64_t k = static_cast<int64_t>(std::floor(beat / s)) - 1;
    while (boundary(k) < beat - kEps) ++k;
    const double sample = bpm_ / 60.0 / static_cast<double>(sr_);
    if (skipFired && std::fabs(lastFireBeat_ - beat) < 1.5 * sample && boundary(k) - beat < 1.5 * sample) {
        ++k;   // that step fired a moment ago (a loop's end, the last sample before a regrid)
    }
    next_ = k;
    lastFired_ = k - 1;
}

double NoteGen::boundary(int64_t k) const {
    const double s = kSyncBeats[p_.rate];
    return static_cast<double>(k) * s + ((k & 1) ? p_.swing * s : 0.0);
}

void NoteGen::startClock() {
    if (playing_) return;   // MPC's bar position rules: the first step lands on its grid
    rebase(0.0);            // stopped: the phrase starts now, at step 1
    beat_ = 0.0;
    next_ = 0;
    lastFired_ = -1;
}

void NoteGen::releaseHeard(Synth& synth) {
    for (int i = 0; i < nHeard_; ++i) synth.noteOff(heard_[i]);
    nHeard_ = 0;
}

void NoteGen::keyOn(int note, int vel, Synth& synth) {
    if (p_.record && p_.mode == SQ_SEQ) {   // the keys write steps instead of playing the sequence
        // While MPC plays (or the sequence runs) at the playing step, else step by step.
        const bool live = (running_ || playing_) && lastStep_ >= 0;
        const int step = wrap(live ? lastStep_ : recPos_);
        if (nRec_ < 16) rec_[nRec_++] = {step, std::clamp(note - 60, -24, 24), std::clamp(vel, 1, 127)};
        if (!running_) {
            if (!live) recPos_ = wrap(step + 1);
            // Hear what you write. Each heard note is listed once, so its key-up always ends it:
            // the same key again ends the old note first, a 17th ends the oldest.
            for (int i = 0; i < nHeard_; ++i)
                if (heard_[i] == note) {
                    synth.noteOff(note);
                    heard_[i] = heard_[--nHeard_];
                    break;
                }
            if (nHeard_ == 16) {
                synth.noteOff(heard_[0]);
                for (int j = 1; j < 16; ++j) heard_[j - 1] = heard_[j];
                --nHeard_;
            }
            synth.noteOn(note, vel);
            heard_[nHeard_++] = note;
        }
        return;
    }
    const bool wasDown = keysDown();
    for (int i = 0; i < nKeys_; ++i)
        if (keys_[i].note == note) {   // a key twice: keep the newer
            for (int j = i + 1; j < nKeys_; ++j) keys_[j - 1] = keys_[j];
            --nKeys_;
            break;
        }
    if (nKeys_ == 16) {
        for (int j = 1; j < 16; ++j) keys_[j - 1] = keys_[j];
        --nKeys_;
    }
    keys_[nKeys_++] = {note, vel, false};
    if (p_.mode == SQ_OFF) {
        synth.noteOn(note, vel);
        if (!wasDown) startClock();   // the step lane still runs as a mod source
        return;
    }
    if (p_.latch) {
        if (latchFresh_) {   // the first key after all were up: a new chord, from its start
            nLatched_ = 0;
            arpIndex_ = 0;
        }
        latchFresh_ = false;
        bool have = false;
        for (int i = 0; i < nLatched_; ++i)
            if (latched_[i].note == note) {   // already in the chord: no second copy
                latched_[i].vel = vel;
                have = true;
            }
        if (!have && nLatched_ < 16) latched_[nLatched_++] = {note, vel, false};
    }
    if (!wasDown) {
        startClock();
        arpIndex_ = 0;
    }
    running_ = true;
    // A fresh phrase's first step is due now; it fires on the next render (one sample on), so
    // every key of a chord struck at this sample is in it.
}

void NoteGen::keyOff(int note, Synth& synth) {
    for (int i = 0; i < nHeard_; ++i)
        if (heard_[i] == note) {   // heard while recording: ends whatever the mode is now
            synth.noteOff(note);
            heard_[i] = heard_[--nHeard_];
            break;
        }
    for (int i = 0; i < nKeys_; ++i)
        if (keys_[i].note == note) {
            if (pedal_ && p_.mode != SQ_OFF) {   // the pedal keeps it in the arp
                keys_[i].pedal = true;
                return;
            }
            for (int j = i + 1; j < nKeys_; ++j) keys_[j - 1] = keys_[j];
            --nKeys_;
            break;
        }
    if (p_.mode == SQ_OFF) {
        synth.noteOff(note);
        return;
    }
    if (nKeys_ == 0) latchFresh_ = true;
    running_ = keysDown();   // the notes already playing finish their gate
}

void NoteGen::pedal(bool down, Synth& synth) {
    pedal_ = down;
    synth.sustain(down && p_.mode == SQ_OFF);
    if (down) return;
    int w = 0;   // pedal up: the keys it held go
    for (int i = 0; i < nKeys_; ++i)
        if (!keys_[i].pedal) keys_[w++] = keys_[i];
    nKeys_ = w;
    if (p_.mode == SQ_OFF) return;
    if (nKeys_ == 0) latchFresh_ = true;
    running_ = keysDown();
}

void NoteGen::panic(Synth& synth) {
    nKeys_ = 0;
    nLatched_ = 0;
    latchFresh_ = true;
    stopGenerated(synth);
    releaseHeard(synth);
    running_ = false;
}

void NoteGen::stopGenerated(Synth& synth) {
    for (int i = 0; i < nOffs_; ++i) synth.noteOff(offs_[i].note);
    nOffs_ = 0;
}

int NoteGen::untilNext(int limit) const {
    const double spb = static_cast<double>(sr_) * 60.0 / bpm_;   // samples per beat
    double t = 1e30;
    if (running_ || playing_ || keysDown()) t = boundary(next_);   // steps, and the Seq lane's edges
    for (int i = 0; i < nOffs_; ++i) t = std::min(t, offs_[i].at);
    if (t >= 1e29) return limit;
    const double samples = std::ceil((t - beat_) * spb - 1e-6);
    if (std::isnan(samples)) return limit;
    return static_cast<int>(std::clamp(samples, 1.0, static_cast<double>(limit)));
}

void NoteGen::advance(int samples, Synth& synth) {
    beat_ += static_cast<double>(samples) * bpm_ / 60.0 / static_cast<double>(sr_);
    const double now = beat_ + 1e-7;
    // Note ends first: a full-length (gate 100%) note ends as the next one starts.
    for (int i = 0; i < nOffs_;) {
        if (offs_[i].at <= now) {
            synth.noteOff(offs_[i].note);
            offs_[i] = offs_[--nOffs_];
        } else {
            ++i;
        }
    }
    if (boundary(next_) < now - 2.0 * kSyncBeats[p_.rate]) reaim(now, false);   // far behind: no burst of steps
    if (!running_) {   // no notes: keep the grid position (and the Seq lane) moving
        while (boundary(next_) <= now) {
            lastFired_ = next_;
            lastStep_ = wrap(next_++);
            if (playing_ || keysDown()) seqValue_ = p_.mod[lastStep_];
        }
        return;
    }
    while (boundary(next_) <= now) {
        const int64_t k = next_++;
        if (k <= lastFired_) continue;
        if (boundary(next_) <= now) {   // a later step is due as well (after a jump): only the latest plays
            lastFired_ = k;
            continue;
        }
        fireStep(k, synth);
    }
}

int NoteGen::arpNotes(int* out, int max) const {
    const bool latched = p_.latch && nLatched_ > 0;   // latched: the whole chord, keys up or down
    const Key* src = latched ? latched_ : keys_;
    const int n = latched ? nLatched_ : nKeys_;
    int base[16];
    for (int i = 0; i < n; ++i) base[i] = src[i].note;
    if (p_.dir != AD_PLAYED) std::sort(base, base + n);
    int up[64];
    int m = 0;
    for (int o = 0; o < p_.octaves; ++o)
        for (int i = 0; i < n && m < 64; ++i) up[m++] = base[i] + 12 * o;
    int c = 0;
    auto push = [&](int v) { if (c < max) out[c++] = v; };
    switch (p_.dir) {
        case AD_DOWN:
            for (int i = m - 1; i >= 0; --i) push(up[i]);
            break;
        case AD_UP_DOWN:   // 1 2 3 2 | 1 2 3 2 ...: the ends play once
            for (int i = 0; i < m; ++i) push(up[i]);
            for (int i = m - 2; i >= 1; --i) push(up[i]);
            break;
        case AD_DOWN_UP:
            for (int i = m - 1; i >= 0; --i) push(up[i]);
            for (int i = 1; i <= m - 2; ++i) push(up[i]);
            break;
        default:
            for (int i = 0; i < m; ++i) push(up[i]);
            break;
    }
    return c;
}

void NoteGen::fireStep(int64_t k, Synth& synth) {
    lastFired_ = k;
    lastFireBeat_ = beat_;
    const int step = wrap(k);
    lastStep_ = step;
    seqValue_ = p_.mod[step];   // the Seq lane moves with the step, not a block later
    if (!keysDown()) return;
    const double len = (boundary(k + 1) - boundary(k)) * static_cast<double>(p_.gate);
    const bool latched = p_.latch && nLatched_ > 0;
    const Key* src = latched ? latched_ : keys_;
    const int n = latched ? nLatched_ : nKeys_;
    if (n == 0) return;
    const Key& last = src[n - 1];
    auto play = [&](int note, int vel) {
        note = std::clamp(note, 0, 127);
        for (int i = 0; i < nOffs_; ++i)   // still sounding from an earlier step: end it now, or
            if (offs_[i].note == note) {   // its pending end would cut this new one short
                synth.noteOff(note);
                offs_[i] = offs_[--nOffs_];
                break;
            }
        if (nOffs_ == 64) return;
        synth.noteOn(note, std::clamp(vel, 1, 127));
        offs_[nOffs_++] = {note, boundary(k) + len};
    };

    if (p_.mode == SQ_SEQ) {   // the pattern, transposed by the newest key
        if (p_.vel[step] <= 0) return;
        play(last.note + p_.note[step], p_.vel[step]);
        return;
    }
    // Arp: the pattern (if on) adds rests, accents and transposition to every step.
    int transpose = 0, vel = last.vel;
    if (p_.pattern) {
        if (p_.vel[step] <= 0) return;   // a rest: the note order doesn't move
        transpose = p_.note[step];
        vel = p_.vel[step];
    }
    int notes[128];
    const int count = arpNotes(notes, 128);
    if (count == 0) return;
    if (p_.dir == AD_CHORD) {
        for (int i = 0; i < count; ++i) play(notes[i] + transpose, vel);
        return;
    }
    const int pick = p_.dir == AD_RANDOM ? static_cast<int>(random() % static_cast<uint32_t>(count))
                                         : static_cast<int>(arpIndex_ % count);
    ++arpIndex_;
    play(notes[pick] + transpose, vel);
}

void NoteGen::shapeValues(float* out4) const {
    const double pos = beat_ / kSyncBeats[p_.shapeRate];
    const double fl = std::floor(pos);
    const int64_t ns = p_.shapeSteps;
    const int i = static_cast<int>(((static_cast<int64_t>(fl) % ns) + ns) % ns);   // count-in: negative beats
    const int j = (i + 1) % p_.shapeSteps;
    const float frac = static_cast<float>(pos - fl);
    for (int l = 0; l < kShapeLanes; ++l) {
        const float a = p_.shape[l][i], b = p_.shape[l][j];
        float v = a;
        if (p_.shapeMode[l] == SM_RAMP) v = a + (b - a) * frac;
        else if (p_.shapeMode[l] == SM_SMOOTH) v = a + (b - a) * (0.5f - 0.5f * std::cos(kPi * frac));
        out4[l] = std::clamp(v, -1.0f, 1.0f);
    }
}

bool NoteGen::takeRecorded(Recorded& r) {
    if (nRec_ == 0) return false;
    r = rec_[0];
    for (int i = 1; i < nRec_; ++i) rec_[i - 1] = rec_[i];
    --nRec_;
    return true;
}

} // namespace pf
