#include "notegen.h"

#include <algorithm>
#include <cmath>

namespace pf {
namespace {
constexpr double kEps = 1e-9;
constexpr float kPi = 3.14159265f;
}

NoteGen::NoteGen(float sampleRate) : sr_(sampleRate) {}

uint32_t NoteGen::random() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
}

void NoteGen::setPatch(const SeqPatch& p, Synth& synth) {
    const int before = p_.mode;
    p_ = p;
    p_.octaves = std::clamp(p_.octaves, 1, 4);
    p_.steps = std::clamp(p_.steps, 1, kSeqSteps);
    p_.shapeSteps = std::clamp(p_.shapeSteps, 1, kShapeSteps);
    p_.rate = std::clamp(p_.rate, 0, kNumSyncDivs - 1);
    p_.shapeRate = std::clamp(p_.shapeRate, 0, kNumSyncDivs - 1);
    p_.gate = std::clamp(p_.gate, 0.05f, 1.0f);
    p_.swing = std::clamp(p_.swing, 0.0f, 0.5f);
    if (p_.mode != before) {
        stopGenerated(synth);
        if (before == SQ_OFF)   // the keys were playing straight through: they stop too
            for (int i = 0; i < nKeys_; ++i) synth.noteOff(keys_[i].note);
        arpIndex_ = 0;
    }
    if (!p_.latch) nLatched_ = 0;
    running_ = p_.mode != SQ_OFF && keysDown();

    // The mod lanes, for this block.
    const float stepBeats = kSyncBeats[p_.rate];
    int idx = -1;
    if (running_ && lastStep_ >= 0) idx = lastStep_;
    else if (playing_ || keysDown()) idx = static_cast<int>(std::floor(beat_ / stepBeats)) % p_.steps;
    seqValue_ = idx >= 0 ? std::clamp(p_.mod[idx], -1.0f, 1.0f) : 0.0f;
}

void NoteGen::setTransport(double bpm, double beats, bool playing, bool beatsValid) {
    bpm_ = bpm > 1.0 ? bpm : 120.0;
    if (playing && beatsValid) {
        const double stepBeats = kSyncBeats[p_.rate];
        // Started, or jumped (a loop, a locate): re-aim at the first step at or after here.
        if (!playing_ || beats < beat_ - 0.5 * stepBeats || beats > beat_ + 2.0 * stepBeats) {
            long k = static_cast<long>(std::floor(beats / stepBeats));
            while (boundary(k) < beats - kEps) ++k;
            next_ = k;
            lastFired_ = k - 1;
        }
        beat_ = beats;
        playing_ = true;
    } else {
        playing_ = false;
    }
}

double NoteGen::boundary(long k) const {
    const double s = kSyncBeats[p_.rate];
    return static_cast<double>(k) * s + ((k & 1) ? p_.swing * s : 0.0);
}

void NoteGen::startClock() {
    if (playing_) return;   // MPC's bar position rules: the first step lands on its grid
    beat_ = 0.0;            // stopped: the phrase starts now, at step 1
    next_ = 0;
    lastFired_ = -1;
}

void NoteGen::keyOn(int note, int vel, Synth& synth) {
    if (p_.record && p_.mode == SQ_SEQ) {   // the keys write steps instead of playing the sequence
        const int step = running_ && lastStep_ >= 0 ? lastStep_ : recPos_;
        if (nRec_ < 16) rec_[nRec_++] = {step, std::clamp(note - 60, -24, 24), std::clamp(vel, 1, 127)};
        if (!running_) {
            recPos_ = (recPos_ + 1) % p_.steps;
            synth.noteOn(note, vel);   // hear what you write
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
    keys_[nKeys_++] = {note, vel};
    if (p_.mode == SQ_OFF) {
        synth.noteOn(note, vel);
        if (!wasDown) startClock();   // the step lane still runs as a mod source
        return;
    }
    if (p_.latch) {
        if (latchFresh_) nLatched_ = 0;   // the first key after all were up: a new chord
        latchFresh_ = false;
        if (nLatched_ < 16) latched_[nLatched_++] = {note, vel};
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
    if (p_.record && p_.mode == SQ_SEQ && !running_) {
        synth.noteOff(note);
        return;
    }
    for (int i = 0; i < nKeys_; ++i)
        if (keys_[i].note == note) {
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

void NoteGen::panic(Synth& synth) {
    nKeys_ = 0;
    nLatched_ = 0;
    latchFresh_ = true;
    stopGenerated(synth);
    running_ = false;
}

void NoteGen::stopGenerated(Synth& synth) {
    for (int i = 0; i < nOffs_; ++i) synth.noteOff(offs_[i].note);
    nOffs_ = 0;
}

int NoteGen::untilNext(int limit) const {
    const double spb = static_cast<double>(sr_) * 60.0 / bpm_;   // samples per beat
    double t = 1e30;
    if (running_) t = boundary(next_);
    for (int i = 0; i < nOffs_; ++i) t = std::min(t, offs_[i].at);
    if (t >= 1e29) return limit;
    const double samples = std::ceil((t - beat_) * spb - 1e-6);
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
    if (!running_) {
        if (boundary(next_) <= now) {   // keep the grid position for when it starts again
            next_ = static_cast<long>(std::floor(beat_ / kSyncBeats[p_.rate])) + 1;
            lastFired_ = next_ - 1;
        }
        return;
    }
    while (boundary(next_) <= now) {
        const long k = next_++;
        if (k <= lastFired_) continue;
        fireStep(k, synth);
    }
}

int NoteGen::arpNotes(int* out, int max) const {
    const Key* src = nKeys_ > 0 ? keys_ : latched_;
    const int n = nKeys_ > 0 ? nKeys_ : nLatched_;
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

void NoteGen::fireStep(long k, Synth& synth) {
    lastFired_ = k;
    const int step = static_cast<int>(((k % p_.steps) + p_.steps) % p_.steps);
    lastStep_ = step;
    if (!keysDown()) return;
    const double len = (boundary(k + 1) - boundary(k)) * static_cast<double>(p_.gate);
    const Key* src = nKeys_ > 0 ? keys_ : latched_;
    const int n = nKeys_ > 0 ? nKeys_ : nLatched_;
    const Key& last = src[n - 1];
    auto play = [&](int note, int vel) {
        note = std::clamp(note, 0, 127);
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
    const int i = static_cast<int>(static_cast<long>(fl) % p_.shapeSteps);
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
