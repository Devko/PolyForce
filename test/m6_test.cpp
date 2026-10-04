// Milestone 6: the arpeggiator, the 16-step sequencer (and its recorder), the shape
// sequencer, transport sync. NoteGen and Synth driven the way the plugin drives them.
#include "host.h"
#include "../dsp/notegen.h"
#include "../dsp/synth.h"

#include <set>

namespace pft {
namespace {

constexpr double kStep16 = 44100.0 * 0.125;   // a 1/16 at 120 BPM, in samples

struct Event { double t; int note; };   // a note starting, in samples

struct Rig {
    pf::Synth synth;
    pf::NoteGen gen;
    pf::SeqPatch seq;
    double now = 0.0;
    std::vector<Event> ons;
    std::vector<Event> offs;
    std::multiset<int> gated;

    Rig() {
        pf::Patch p;
        p.env[0] = {0.001f, 0.1f, 1.0f, 0.01f};
        p.voices = 8;
        synth.setPatch(p);
        seq.mode = pf::SQ_ARP;
    }
    void scan() {
        std::multiset<int> g;
        for (int i = 0; i < pf::kMaxVoices; ++i) {
            const auto v = synth.voiceInfo(i);
            if (v.active && v.gate) g.insert(v.note);
        }
        for (int n : g)
            if (g.count(n) > gated.count(n)) ons.push_back({now, n});
        for (int n : gated)
            if (gated.count(n) > g.count(n)) offs.push_back({now, n});
        gated = g;
    }
    // One block, the plugin's loop: render up to each due event, advance, repeat.
    void block(double bpm = 120.0, double beats = 0.0, bool playing = false) {
        gen.setTransport(bpm, beats, playing, playing);
        gen.setPatch(seq, synth);
        gen.advance(0, synth);
        scan();
        float L[kBlock], R[kBlock];
        int pos = 0;
        while (pos < kBlock) {
            const int step = gen.untilNext(kBlock - pos);
            synth.render(L + pos, R + pos, step);
            pos += step;
            now += step;
            gen.advance(step, synth);
            scan();
        }
    }
    void run(double seconds) {
        const int blocks = static_cast<int>(seconds * 44100.0 / kBlock);
        for (int b = 0; b < blocks; ++b) block();
    }
    void key(int note, int vel = 100) {
        gen.setPatch(seq, synth);
        gen.keyOn(note, vel, synth);
        scan();
    }
    void up(int note) {
        gen.keyOff(note, synth);
        scan();
    }
    std::vector<int> order(size_t n) const {
        std::vector<int> out;
        for (size_t i = 0; i < ons.size() && out.size() < n; ++i) out.push_back(ons[i].note);
        return out;
    }
};

void arpDirections() {
    struct Case { int dir, octaves; std::vector<int> expect; };
    const Case cases[] = {
        {pf::AD_UP, 1, {60, 64, 67, 60, 64, 67}},
        {pf::AD_DOWN, 1, {67, 64, 60, 67, 64, 60}},
        {pf::AD_UP_DOWN, 1, {60, 64, 67, 64, 60, 64}},
        {pf::AD_DOWN_UP, 1, {67, 64, 60, 64, 67, 64}},
        {pf::AD_UP, 2, {60, 64, 67, 72, 76, 79, 60}},
    };
    for (const Case& c : cases) {
        Rig r;
        r.seq.dir = c.dir;
        r.seq.octaves = c.octaves;
        for (int n : {64, 60, 67}) r.key(n);   // pressed out of order: Up sorts them
        r.ons.clear();
        r.run(1.0);
        CHECK(r.order(c.expect.size()) == c.expect);
    }
    // Played: in the order the keys went down.
    {
        Rig r;
        r.seq.dir = pf::AD_PLAYED;
        for (int n : {64, 60, 67}) r.key(n);
        r.ons.clear();
        r.run(1.0);
        CHECK((r.order(4) == std::vector<int>{64, 60, 67, 64}));
    }
    // Chord: every held note on every step; Random: only held notes.
    {
        Rig r;
        r.seq.dir = pf::AD_CHORD;
        for (int n : {60, 64, 67}) r.key(n);
        r.run(0.3);
        int at0 = 0;
        for (const Event& e : r.ons) at0 += e.t == r.ons.front().t ? 1 : 0;
        CHECK(at0 == 3);
    }
    {
        Rig r;
        r.seq.dir = pf::AD_RANDOM;
        for (int n : {60, 64, 67}) r.key(n);
        r.run(2.0);
        bool ok = r.ons.size() >= 15;
        for (const Event& e : r.ons) ok = ok && (e.note == 60 || e.note == 64 || e.note == 67);
        CHECK(ok);
    }
}

void timing() {
    // 1/16 at 120 BPM: one step every 5512.5 samples; the first at the key; gate 50%.
    {
        Rig r;
        r.key(60);
        r.run(1.0);
        CHECK(r.ons.size() >= 8);
        CHECK(r.ons[0].t == 0.0);
        CHECK(std::fabs(r.ons[1].t - kStep16) <= 1.0 && std::fabs(r.ons[4].t - 4 * kStep16) <= 1.0);
        CHECK(!r.offs.empty() && std::fabs(r.offs[0].t - 0.5 * kStep16) <= 1.0);
    }
    // Swing 50%: every second step half a step late.
    {
        Rig r;
        r.seq.swing = 0.5f;
        r.key(60);
        r.run(1.0);
        CHECK(std::fabs(r.ons[1].t - 1.5 * kStep16) <= 1.0 && std::fabs(r.ons[2].t - 2 * kStep16) <= 1.0);
    }
    // Transport: steps land on MPC's grid. At beat 0.6 the next 1/16 is at beat 0.75.
    {
        Rig r;
        r.block(120.0, 0.6, true);
        r.key(60);   // no step at the key: the grid decides
        CHECK(r.ons.empty());
        double beats = 0.6 + kBlock / 22050.0;
        for (int b = 0; b < 40; ++b, beats += kBlock / 22050.0) r.block(120.0, beats, true);
        CHECK(!r.ons.empty() && std::fabs(r.ons[0].t - (0.15 * 22050.0 + kBlock)) <= kBlock + 1.0);
    }
}

void latchPatternSeq() {
    // Latch: the arp keeps going after the keys are up; a new key after that starts a new chord.
    {
        Rig r;
        r.seq.latch = true;
        r.key(60);
        r.key(64);
        r.up(60);
        r.up(64);
        r.ons.clear();
        r.run(0.6);
        CHECK(r.ons.size() >= 4);
        r.key(72);
        r.up(72);
        r.ons.clear();
        r.run(0.6);
        bool only72 = !r.ons.empty();
        for (const Event& e : r.ons) only72 = only72 && e.note == 72;
        CHECK(only72);
    }
    // Pattern: step 2 rests, step 3 an octave up.
    {
        Rig r;
        r.seq.pattern = true;
        r.seq.vel[1] = 0;
        r.seq.note[2] = 12;
        r.seq.steps = 4;
        r.key(60);
        r.run(0.9);
        CHECK(r.ons.size() >= 5);
        CHECK(std::fabs(r.ons[1].t - 2 * kStep16) <= 1.0);   // step 2 silent
        CHECK(r.ons[1].note == 72);
    }
    // Seq: the steps transposed by the key, wrapping after `steps`.
    {
        Rig r;
        r.seq.mode = pf::SQ_SEQ;
        r.seq.steps = 3;
        r.seq.note[0] = 0;
        r.seq.note[1] = 7;
        r.seq.note[2] = 12;
        r.key(48);
        r.run(0.9);
        CHECK((r.order(6) == std::vector<int>{48, 55, 60, 48, 55, 60}));
        r.up(48);
        r.ons.clear();
        r.run(0.5);
        CHECK(r.ons.empty());   // the key is up: it stops
        CHECK(r.gated.empty());
    }
    // Off: keys play straight through, no steps.
    {
        Rig r;
        r.seq.mode = pf::SQ_OFF;
        r.key(60);
        r.run(0.5);
        CHECK(r.ons.size() == 1 && r.gated.count(60) == 1);
        r.up(60);
        CHECK(r.gated.empty());
    }
}

void shapes() {
    pf::NoteGen g;
    pf::Synth s;
    pf::SeqPatch p;
    p.shapeRate = 9;   // 1/8 = 0.5 beats
    p.shapeSteps = 4;
    p.shape[0][0] = -1.0f;
    p.shape[0][1] = 1.0f;
    p.shape[0][2] = 0.5f;
    p.shape[0][3] = 0.0f;
    p.shapeMode[1] = pf::SM_RAMP;
    p.shape[1][0] = 0.0f;
    p.shape[1][1] = 1.0f;
    float v[4];
    auto at = [&](double beat) {
        g.setTransport(120.0, beat, true, true);
        g.setPatch(p, s);
        g.shapeValues(v);
    };
    at(0.0);
    CHECK(v[0] == -1.0f && v[1] == 0.0f);
    at(0.5);
    CHECK(v[0] == 1.0f && v[1] == 1.0f);
    at(0.25);
    CHECK(v[0] == -1.0f && std::fabs(v[1] - 0.5f) < 1e-4f);   // Step holds, Ramp halfway
    at(2.0 + 0.5 * 2);   // 4 steps of 0.5 beats wrap every 2 beats: beat 3 = step 3
    CHECK(v[0] == 0.5f);
}

void plugin() {
    // Through MIDI: the arp's first note starts at the key's own sample, steps follow.
    Host h;
    h.bare();
    h.set(pf::P_SEQ_MODE, pf::SQ_ARP);
    h.set(pf::P_E1_R, 0.001f);
    h.on(69, 127, 40);
    h.run(8);
    float before = 0.0f;
    for (int i = 0; i < 40; ++i) before = std::max(before, std::fabs(h.L[static_cast<size_t>(i)]));
    CHECK(before == 0.0f && h.finite);
    float after = 0.0f;
    for (int i = 42; i < 200; ++i) after = std::max(after, std::fabs(h.L[static_cast<size_t>(i)]));
    CHECK(after > 1e-5f);
    // The recorder: Seq + Rec, stopped: two keys write steps 1 and 2.
    Host r;
    r.set(pf::P_SEQ_MODE, pf::SQ_SEQ);
    r.setN(pf::P_SEQ_REC, 1.0f);
    r.on(62, 90);
    r.off(62);
    r.on(53, 70);
    r.off(53);
    r.run(4);
    CHECK(r.value(pf::P_S1_NOTE) == 2 && r.value(pf::P_S1_VEL) == 90);
    CHECK(r.value(pf::P_S2_NOTE) == -7 && r.value(pf::P_S2_VEL) == 70);
    CHECK(r.log.automated.count(pf::P_S2_NOTE) > 0);   // MPC hears about the written steps
    CHECK(r.chunk().find("s2_note=-7\n") != std::string::npos);
    // The Seq lane as a mod source (volume), running without notes: arp/seq Off, a key held.
    Host m;
    m.bare();
    m.set(pf::P_M1_SRC, pf::MS_SEQ);
    m.set(pf::P_M1_T1, pf::MT_VOLUME);
    m.set(pf::P_M1_A1, -1.0f);
    for (int k = 0; k < 16; ++k) m.set(pf::P_S1_MOD + k, k % 2 ? 1.0f : 0.0f);   // loud, silent, loud...
    m.on(69, 127);
    m.run(kBlocksPerSec);
    int changes = 0;
    bool loud = true;
    for (size_t i = 0; i + 441 < m.L.size(); i += 441) {
        float pk = 0.0f;
        for (size_t j = i; j < i + 441; ++j) pk = std::max(pk, std::fabs(m.L[j]));
        const bool now = pk > 0.01f;
        changes += now != loud ? 1 : 0;
        loud = now;
    }
    CHECK(changes >= 6 && changes <= 10);   // 8 steps a second (1/16 at 120 BPM), each a change
    CHECK(m.display(pf::P_CLK_RATE) == "1/16" && m.display(pf::P_ARP_DIR) == "Up");
}

} // namespace

void sequencerTests() {
    arpDirections();
    timing();
    latchPatternSeq();
    shapes();
    plugin();
}

} // namespace pft
