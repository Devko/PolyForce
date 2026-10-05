// Milestone 2: voices. Steal modes, the click-free steal, Poly/Duo/Mono/Legato, glide,
// the same note again, bend ranges, the velocity curve. Mostly on the engine directly.
#include "host.h"
#include "../dsp/synth.h"
#include "../plugin/cpu_guard.h"

#include <set>

namespace pft {
namespace {

// A clean, steady voice: one sine oscillator, filters off, a 1 ms attack, full sustain.
pf::Patch plain() {
    pf::Patch p;
    p.osc[0].pos = 0.0f;   // Classic frame 0: sine
    p.osc[0].unison = 1;
    p.osc[1].level = 0.0f;
    p.flt[0].type = pf::F_OFF;
    p.flt[1].type = pf::F_OFF;
    p.env[0] = {0.001f, 0.1f, 1.0f, 0.05f};
    p.velSens = 0.0f;
    return p;
}

struct Rig {
    pf::Synth s;
    float L[kBlock], R[kBlock];
    float peak = 0.0f, maxJump = 0.0f, last = 0.0f;
    explicit Rig(const pf::Patch& p) { s.setPatch(p); }
    void run(int blocks) {
        peak = 0.0f;
        for (int b = 0; b < blocks; ++b) {
            s.render(L, R, kBlock);
            for (int i = 0; i < kBlock; ++i) {
                peak = std::max(peak, std::fabs(L[i]));
                maxJump = std::max(maxJump, std::fabs(L[i] - last));
                last = L[i];
            }
        }
    }
    std::multiset<int> gated() const {
        std::multiset<int> out;
        for (int i = 0; i < pf::kMaxVoices; ++i) {
            const auto v = s.voiceInfo(i);
            if (v.active && v.gate) out.insert(v.note);
        }
        return out;
    }
    int sounding() const {
        int n = 0;
        for (int i = 0; i < pf::kMaxVoices; ++i) n += s.voiceInfo(i).active ? 1 : 0;
        return n;
    }
    bool sounds(int note) const {
        for (int i = 0; i < pf::kMaxVoices; ++i) {
            const auto v = s.voiceInfo(i);
            if (v.active && v.note == note) return true;
        }
        return false;
    }
    float pitchOf(int note) const {
        for (int i = 0; i < pf::kMaxVoices; ++i) {
            const auto v = s.voiceInfo(i);
            if (v.active && v.note == note) return v.pitch;
        }
        return -1.0f;
    }
};

void stealModes() {
    // Oldest: the first note goes.
    {
        pf::Patch p = plain();
        p.voices = 4;
        Rig r(p);
        for (int n : {60, 62, 64, 65}) { r.s.noteOn(n, 100); r.run(2); }
        r.s.noteOn(67, 100);
        r.run(4);   // past the 3 ms fade
        CHECK((r.gated() == std::multiset<int>{62, 64, 65, 67}));
    }
    // Keep low / keep high: the bass (top) note survives a stream of chords over it.
    for (int mode : {pf::ST_KEEP_LOW, pf::ST_KEEP_HIGH}) {
        pf::Patch p = plain();
        p.voices = 4;
        p.steal = mode;
        Rig r(p);
        const int kept = mode == pf::ST_KEEP_LOW ? 36 : 96;
        r.s.noteOn(kept, 100);
        r.run(2);
        for (int n = 60; n < 69; ++n) { r.s.noteOn(n, 100); r.run(4); }
        CHECK(r.gated().count(kept) == 1);
        CHECK(r.gated().size() == 4);
    }
    // Quietest: a released voice goes before any held one, the quietest of them first.
    {
        pf::Patch p = plain();
        p.voices = 4;
        p.steal = pf::ST_QUIETEST;
        p.env[0].r = 2.0f;   // long tails: released voices are still sounding
        Rig r(p);
        for (int n : {60, 62, 64, 65}) { r.s.noteOn(n, 100); r.run(2); }
        r.s.noteOff(62);
        r.run(40);           // 62 has decayed further than 65 will have
        r.s.noteOff(65);
        r.run(2);
        r.s.noteOn(70, 100);
        r.run(4);
        const auto g = r.gated();
        CHECK(g.count(60) && g.count(64) && g.count(70));   // the held ones survive
        bool has62 = false, has65 = false;
        for (int i = 0; i < pf::kMaxVoices; ++i) {
            const auto v = r.s.voiceInfo(i);
            has62 = has62 || (v.active && v.note == 62);
            has65 = has65 || (v.active && v.note == 65);
        }
        CHECK(!has62 && has65);   // the quieter tail was taken
    }
}

void clickFreeSteal() {
    pf::Patch p = plain();
    p.voices = 1;
    Rig r(p);
    r.s.noteOn(69, 127);
    r.run(40);
    const float natural = r.maxJump;   // a 440 Hz sine's own largest step
    r.maxJump = 0.0f;
    r.s.noteOn(76, 127);   // steals the only voice mid-cycle
    r.run(40);
    CHECK(r.maxJump < 1.6f * natural);   // faded out and restarted from silence: no step
    CHECK((r.gated() == std::multiset<int>{76}));
    CHECK(r.peak > 0.05f);
}

void monoLegatoDuo() {
    // Mono: one voice, back to the held key on release.
    {
        pf::Patch p = plain();
        p.voiceMode = pf::VM_MONO;
        Rig r(p);
        r.s.noteOn(60, 100);
        r.run(4);
        r.s.noteOn(64, 100);
        r.run(4);
        CHECK(r.sounding() == 1 && r.pitchOf(64) == 64.0f);
        r.s.noteOff(64);
        r.run(4);
        CHECK(r.sounding() == 1 && r.pitchOf(60) == 60.0f);
        r.s.noteOff(60);
        r.run(40);
        CHECK(r.sounding() == 0);
    }
    // Mono re-attacks on a new key, Legato doesn't: decay to a low sustain, then overlap.
    float peaks[2];
    for (int legato = 0; legato < 2; ++legato) {
        pf::Patch p = plain();
        p.voiceMode = legato ? pf::VM_LEGATO : pf::VM_MONO;
        p.env[0] = {0.001f, 0.05f, 0.2f, 0.05f};
        Rig r(p);
        r.s.noteOn(60, 127);
        r.run(80);   // settled on the sustain level
        r.s.noteOn(67, 127);
        r.run(8);
        peaks[legato] = r.peak;
    }
    CHECK(peaks[0] > 2.5f * peaks[1]);
    // Duo: the two newest keys sound; a released key's voice takes over a held one.
    {
        pf::Patch p = plain();
        p.voiceMode = pf::VM_DUO;
        Rig r(p);
        for (int n : {60, 64, 67}) { r.s.noteOn(n, 100); r.run(4); }
        CHECK((r.gated() == std::multiset<int>{64, 67}));
        r.s.noteOff(67);
        r.run(4);
        CHECK((r.gated() == std::multiset<int>{60, 64}));
    }
}

void glide() {
    // Time: every glide takes glideTime, linear in pitch.
    {
        pf::Patch p = plain();
        p.voiceMode = pf::VM_MONO;
        p.glideMode = pf::GL_ALWAYS;
        p.glideTime = 0.2f;
        Rig r(p);
        r.s.noteOn(60, 100);
        r.run(4);
        r.s.noteOn(72, 100);
        r.run(static_cast<int>(0.1 * 44100 / kBlock));   // half way
        const float mid = r.pitchOf(72);
        CHECK(mid > 65.0f && mid < 67.0f);
        r.run(static_cast<int>(0.12 * 44100 / kBlock));
        CHECK(r.pitchOf(72) == 72.0f);
    }
    // Rate: glideTime per octave, so two octaves take twice as long.
    {
        pf::Patch p = plain();
        p.voiceMode = pf::VM_MONO;
        p.glideMode = pf::GL_ALWAYS;
        p.glideRate = true;
        p.glideTime = 0.1f;
        Rig r(p);
        r.s.noteOn(60, 100);
        r.run(4);
        r.s.noteOn(84, 100);
        r.run(static_cast<int>(0.1 * 44100 / kBlock));
        const float mid = r.pitchOf(84);
        CHECK(mid > 71.0f && mid < 73.0f);   // one octave after 0.1 s
    }
    // Legato glide: only between overlapping notes. Poly glide comes from the last note.
    {
        pf::Patch p = plain();
        p.glideMode = pf::GL_LEGATO;
        p.glideTime = 0.5f;
        Rig r(p);
        r.s.noteOn(60, 100);
        r.run(4);
        r.s.noteOff(60);
        r.s.noteOn(72, 100);     // detached: no glide
        r.run(2);
        CHECK(r.pitchOf(72) == 72.0f);
        r.s.noteOn(48, 100);     // overlapping 72: glides from 72 down
        r.run(2);
        const float pg = r.pitchOf(48);
        CHECK(pg > 70.0f && pg < 72.0f);
    }
}

void sameNoteAndRanges() {
    // The same note again: a new voice when asked; key-ups release them one at a time.
    {
        pf::Patch p = plain();
        p.sameNoteNew = true;
        Rig r(p);
        r.s.noteOn(60, 100);
        r.run(2);
        r.s.noteOn(60, 100);
        r.run(2);
        CHECK(r.gated().count(60) == 2);
        r.s.noteOff(60);
        r.run(2);
        CHECK(r.gated().count(60) == 1);
    }
    // Bend ranges (through the plugin): +12 up at full bend makes A4 sound A5.
    {
        Host h;
        h.bare();
        h.set(pf::P_BEND_UP, 12);
        h.midi(0xE0, 0x7F, 0x7F);
        h.on(69, 127);
        h.run(kBlocksPerSec / 4);
        h.run(kBlocksPerSec);
        int rises = 0;
        for (size_t i = 1; i < h.L.size(); ++i) rises += (h.L[i - 1] < 0.0f && h.L[i] >= 0.0f) ? 1 : 0;
        CHECK(std::abs(rises - static_cast<int>(880.0 * h.L.size() / 44100.0)) <= 3);
        CHECK(h.display(pf::P_BEND_UP) == "+12 st");
        CHECK(h.display(pf::P_VMODE) == "Poly" && h.display(pf::P_STEAL) == "Oldest");
    }
    // Velocity curve: soft makes a gentle key louder, hard quieter.
    float level[3];
    const float curves[3] = {-1.0f, 0.0f, 1.0f};
    for (int c = 0; c < 3; ++c) {
        pf::Patch p = plain();
        p.velSens = 1.0f;
        p.velCurve = curves[c];
        Rig r(p);
        r.s.noteOn(69, 40);
        r.run(20);
        level[c] = r.peak;
    }
    CHECK(level[0] < level[1] && level[1] < level[2]);
}

// The CPU guard: the rule (a lone costly block sheds nothing) and the engine side (only
// release tails go, quietest first, without a click; held and sustained notes stay).
void cpuGuard() {
    {
        pf::CpuGuard g;
        const double b = 2902.0;                    // one 128-frame block
        CHECK(g.afterBlock(0.5 * b, b) == 0);       // one block over 40%: a spike
        CHECK(g.afterBlock(0.5 * b, b) == 1);       // the next one too: shed
        CHECK(g.afterBlock(0.1 * b, b) == 0);
        CHECK(g.afterBlock(0.8 * b, b) == 2);       // over 65%: at once
        CHECK(g.afterBlock(0.3 * b, b) == 0);
        // Small calls add up to a block first: process(513) = 4 x 128 frames at 20% plus a
        // 1-frame remainder that pays a whole chunk (100 us: 440% of its own 22.7 us budget).
        pf::CpuGuard s;
        int shed = 0;
        for (int blk = 0; blk < 10; ++blk) {
            for (int c = 0; c < 4; ++c) shed += s.afterBlock(0.2 * b, b);
            shed += s.afterBlock(100.0, b / 128.0);
        }
        CHECK(shed == 0);
        for (int c = 0; c < 256; ++c) shed += s.afterBlock(0.5 * b / 128.0, b / 128.0);   // 1-frame calls at 50%
        CHECK(shed == 1);                           // two full windows over 40%: one tail
    }
    pf::Patch p = plain();
    p.env[0].r = 4.0f;   // long tails
    Rig r(p);
    r.s.noteOn(60, 100);
    r.s.noteOn(64, 100);
    r.s.noteOn(67, 100);
    r.s.noteOn(72, 100);
    r.run(20);
    r.s.noteOff(60);
    r.run(40);           // 60 has been ringing out longest: the quietest tail
    r.s.noteOff(64);
    r.s.sustain(true);
    r.s.noteOff(67);     // held by the pedal: not a tail
    r.run(4);
    CHECK(r.s.activeVoices() == 4);
    CHECK(r.s.shedTails(1) == 1);
    r.maxJump = 0.0f;
    r.run(4);            // the 3 ms fade, then the voice is free
    CHECK(r.s.activeVoices() == 3);
    CHECK(!r.sounds(60) && r.sounds(64) && r.sounds(67) && r.sounds(72));
    CHECK(r.maxJump < 0.05f);   // the sine's own slope ~0.04; cut without the fade: ~0.074 (at -6 dB per voice)
    CHECK(r.s.shedTails(4) == 1);   // only 64 is left to shed
    r.run(4);
    CHECK(r.s.activeVoices() == 2 && r.sounds(67) && r.sounds(72));
    CHECK(r.s.shedTails(4) == 0);

    // Mono: a key pressed while the shed tail fades waits for the fade (no jump back to the
    // tail's level), then plays.
    {
        pf::Patch m = plain();
        m.voiceMode = pf::VM_MONO;
        m.env[0].r = 4.0f;
        Rig r(m);
        r.s.noteOn(48, 100);
        r.run(20);
        r.s.noteOff(48);
        r.run(2);
        CHECK(r.s.shedTails(1) == 1);
        float L[256], R[256];
        r.s.render(L, R, 96);
        r.s.noteOn(55, 100);
        r.s.render(L + 96, R + 96, 160);
        float peak = 0.0f, jump = 0.0f;
        for (int i = 1; i < 256; ++i) {
            peak = std::max(peak, std::fabs(L[i]));
            jump = std::max(jump, std::fabs(L[i] - L[i - 1]));
        }
        CHECK(jump < 0.25f * peak);
        r.run(2);
        CHECK(r.sounds(55) && r.s.voiceInfo(0).gate);
    }
}

} // namespace

void voiceTests() {
    stealModes();
    clickFreeSteal();
    monoLegatoDuo();
    glide();
    sameNoteAndRanges();
    cpuGuard();
}

} // namespace pft
