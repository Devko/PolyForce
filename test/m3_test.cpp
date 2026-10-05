// Milestone 3: oscillators. Classic waves, pulse width, the sub oscillator, phase modes,
// pan, per-source routing into the filters, the noise source.
#include "host.h"
#include "../dsp/synth.h"

namespace pft {
namespace {

pf::Patch base() {
    pf::Patch p;
    p.osc[0].unison = 1;
    p.osc[0].level = 0.8f;
    p.osc[1].level = 0.0f;
    p.flt[0].type = pf::F_OFF;
    p.flt[1].type = pf::F_OFF;
    p.env[0] = {0.001f, 0.1f, 1.0f, 0.05f};
    p.velSens = 0.0f;
    return p;
}

struct Take {
    std::vector<float> L, R;
};

Take play(const pf::Patch& p, int note, int blocks, int skip = 40) {
    pf::Synth s;
    s.setPatch(p);
    s.noteOn(note, 127);
    float l[kBlock], r[kBlock];
    for (int b = 0; b < skip; ++b) s.render(l, r, kBlock);   // past the attack
    Take t;
    for (int b = 0; b < blocks; ++b) {
        s.render(l, r, kBlock);
        t.L.insert(t.L.end(), l, l + kBlock);
        t.R.insert(t.R.end(), r, r + kBlock);
    }
    return t;
}

int rises(const std::vector<float>& x) {
    int n = 0;
    for (size_t i = 1; i < x.size(); ++i) n += (x[i - 1] < 0.0f && x[i] >= 0.0f) ? 1 : 0;
    return n;
}

float rms(const std::vector<float>& x) {
    double s = 0.0;
    for (float v : x) s += static_cast<double>(v) * v;
    return x.empty() ? 0.0f : static_cast<float>(std::sqrt(s / static_cast<double>(x.size())));
}

// Share of the signal's power above ~5 kHz, from first differences (a crude brightness meter).
float brightness(const std::vector<float>& x) {
    double d = 0.0, s = 0.0;
    for (size_t i = 1; i < x.size(); ++i) {
        d += static_cast<double>(x[i] - x[i - 1]) * (x[i] - x[i - 1]);
        s += static_cast<double>(x[i]) * x[i];
    }
    return s > 0 ? static_cast<float>(d / s) : 0.0f;
}

void waves() {
    const int blocks = kBlocksPerSec;   // 1 s
    for (int w : {pf::OW_SINE, pf::OW_TRIANGLE, pf::OW_SAW, pf::OW_SQUARE, pf::OW_PULSE}) {
        pf::Patch p = base();
        p.osc[0].wave = w;
        const Take t = play(p, 69, blocks);
        const int expect = static_cast<int>(440.0 * t.L.size() / 44100.0);
        CHECK(std::abs(rises(t.L) - expect) <= 3);
        CHECK(rms(t.L) > 0.02f);
    }
    // A saw is brighter than a triangle, which is brighter than a sine.
    float bright[3];
    const int order[3] = {pf::OW_SINE, pf::OW_TRIANGLE, pf::OW_SAW};
    for (int i = 0; i < 3; ++i) {
        pf::Patch p = base();
        p.osc[0].wave = order[i];
        bright[i] = brightness(play(p, 57, 20).L);
    }
    CHECK(bright[0] < bright[1] && bright[1] < bright[2]);
    // Pulse width: position 0 = 50% duty, position 1 = 3%.
    for (int k = 0; k < 2; ++k) {
        pf::Patch p = base();
        p.osc[0].wave = pf::OW_PULSE;
        p.osc[0].pos = static_cast<float>(k);
        const Take t = play(p, 45, 40);
        int pos = 0;
        for (float v : t.L) pos += v > 0.0f ? 1 : 0;
        const float duty = static_cast<float>(pos) / static_cast<float>(t.L.size());
        if (k == 0) CHECK(duty > 0.45f && duty < 0.55f);
        else CHECK(duty < 0.12f);
    }
    // Noise as an oscillator: not periodic.
    pf::Patch p = base();
    p.osc[0].wave = pf::OW_NOISE;
    p.osc[0].pos = 0.5f;
    const Take t = play(p, 69, 20);
    CHECK(rises(t.L) > 2000 * 20 * kBlock / 44100);
}

void subOsc() {
    pf::Patch p = base();
    p.osc[0].level = 0.0f;   // the sub alone
    p.osc[0].subLevel = 1.0f;
    p.osc[0].subTune = -12.0f;
    const Take t = play(p, 69, kBlocksPerSec);
    CHECK(std::abs(rises(t.L) - static_cast<int>(220.0 * t.L.size() / 44100.0)) <= 3);
    p.osc[0].subTune = 7.0f;   // tunable: a fifth up
    const Take u = play(p, 69, kBlocksPerSec);
    CHECK(std::abs(rises(u.L) - static_cast<int>(659.26 * u.L.size() / 44100.0)) <= 3);
}

void phases() {
    // Reset: every note starts the same, unison included. Random: they don't.
    pf::Patch p = base();
    p.osc[0].unison = 4;
    const Take a = play(p, 60, 4, 0), b = play(p, 60, 4, 0);
    CHECK(a.L == b.L);
    // Random: two notes on one engine (two voices) start at different phases. Reset: equal.
    for (int mode : {pf::PH_RESET, pf::PH_RANDOM}) {
        pf::Patch q = base();
        q.osc[0].phaseMode = mode;
        pf::Synth s;
        s.setPatch(q);
        int apart = 0;
        for (int trial = 0; trial < 6; ++trial) {
            s.reset();
            s.noteOn(60, 127);
            s.noteOn(72, 127);
            float l[kBlock], r[kBlock];
            s.render(l, r, kBlock);
            // Voice 0 plays 60, voice 1 plays 72 (an octave up): at Reset both start at phase 0,
            // so the first samples rise together; at Random they mostly don't agree.
            const bool together = l[1] > 0.0f && l[2] > l[1];
            if (mode == pf::PH_RESET) CHECK(together);
            apart += together ? 0 : 1;
        }
        if (mode == pf::PH_RANDOM) CHECK(apart >= 1);
    }
    p.osc[0].phaseMode = pf::PH_RANDOM;
    pf::Synth s2;
    s2.setPatch(p);
    std::vector<float> first, second;
    float l[kBlock], r[kBlock];
    s2.noteOn(60, 127);
    s2.render(l, r, kBlock);
    first.assign(l, l + kBlock);
    s2.reset();
    s2.noteOn(60, 127);
    s2.render(l, r, kBlock);
    second.assign(l, l + kBlock);
    CHECK(first != second);
    // The phase knob: a sine started half a cycle later is the same sine upside down.
    pf::Patch q = base();
    q.osc[0].wave = pf::OW_SINE;
    const Take e = play(q, 60, 4, 0);
    q.osc[0].phase = 0.5f;
    const Take f = play(q, 60, 4, 0);
    double dot = 0.0, ee = 0.0;
    for (size_t i = 0; i < e.L.size(); ++i) {
        dot += static_cast<double>(e.L[i]) * f.L[i];
        ee += static_cast<double>(e.L[i]) * e.L[i];
    }
    CHECK(dot < -0.95 * ee);
}

void routingAndPan() {
    // Filter 1 a 20 Hz lowpass: an oscillator routed to it is silenced, one routed Direct isn't.
    pf::Patch p = base();
    p.osc[0].wave = pf::OW_SAW;
    p.flt[0].type = pf::F_LP24;
    p.flt[0].cutoffHz = 20.0f;
    p.flt[0].key = 0.0f;
    p.flt[0].env = 0.0f;
    const float filtered = rms(play(p, 69, 20).L);
    p.osc[0].route = pf::RT_DIRECT;
    const float direct = rms(play(p, 69, 20).L);
    CHECK(filtered < 0.02f * direct);
    // Serial: filter 1 feeds filter 2. Parallel: their outputs are summed.
    p.osc[0].route = pf::RT_F1;
    p.flt[0].type = pf::F_OFF;
    p.flt[1].type = pf::F_LP24;
    p.flt[1].cutoffHz = 20.0f;
    p.flt[1].key = 0.0f;
    p.flt[1].env = 0.0f;
    const float serial = rms(play(p, 69, 20).L);
    p.parallel = true;
    const float parallel = rms(play(p, 69, 20).L);
    CHECK(serial < 0.02f * parallel);
    // F2 only: past filter 1.
    p.parallel = false;
    p.osc[0].route = pf::RT_F2;
    p.flt[0].type = pf::F_LP24;
    p.flt[0].cutoffHz = 20.0f;
    p.flt[1].type = pf::F_OFF;
    CHECK(rms(play(p, 69, 20).L) > 0.5f * direct);
    // Pan hard left: the right channel is (nearly) empty.
    pf::Patch q = base();
    q.osc[0].pan = -1.0f;
    const Take t = play(q, 60, 20);
    CHECK(rms(t.R) < 0.01f * rms(t.L));
}

void noise() {
    float b[3];
    const float colors[3] = {-1.0f, 0.0f, 1.0f};
    for (int i = 0; i < 3; ++i) {
        pf::Patch p = base();
        p.osc[0].level = 0.0f;
        p.noise.level = 1.0f;
        p.noise.color = colors[i];
        const Take t = play(p, 60, 40);
        CHECK(rms(t.L) > 0.02f && rms(t.L) < 1.0f);
        CHECK(t.L != t.R);   // stereo noise
        b[i] = brightness(t.L);
    }
    CHECK(b[0] < b[1] && b[1] < b[2]);
}

void surfaceTexts() {
    Host h;
    h.set(pf::P_O1_WAVE, pf::OW_PULSE);
    h.set(pf::P_O1_POS, 0.0f);
    CHECK(h.display(pf::P_O1_POS) == "WIDTH 50%");
    h.set(pf::P_O1_WAVE, pf::OW_SINE);
    CHECK(h.display(pf::P_O1_POS) == "-");
    h.set(pf::P_O1_WAVE, pf::OW_NOISE);
    h.set(pf::P_O1_POS, 0.5f);
    CHECK(h.display(pf::P_O1_POS) == "WHITE");
    CHECK(h.display(pf::P_O1_WAVE) == "Noise");
    h.set(pf::P_O1_PAN, -0.5f);
    CHECK(h.display(pf::P_O1_PAN) == "L50");
    h.set(pf::P_O1_PHASE, 0.25f);
    CHECK(h.display(pf::P_O1_PHASE) == "90 deg");
    CHECK(h.display(pf::P_O1_SUB_TUNE) == "-12 st");
    // A popup closes on a tap on an option, stays open for a Q-Link nudge.
    h.setN(pf::P_O1_WAVE__OPEN, 1.0f);
    CHECK(h.get(pf::P_O1_WAVE__OPEN) == 1.0f);
    h.setN(pf::P_O1_WAVE, 0.0f);   // a tap on "Table"
    CHECK(h.get(pf::P_O1_WAVE__OPEN) == 0.0f && h.display(pf::P_O1_WAVE) == "Table");
    // Version 3 states: Parallel meant osc 2 -> F2.
    CHECK(h.load("polyforce 3\nrouting=1\n") == 1);
    CHECK(h.display(pf::P_O2_ROUTE) == "F2" && h.display(pf::P_O1_ROUTE) == "F1");
    CHECK(h.load("polyforce 4\nrouting=1\no2_route=0\n") == 1);
    CHECK(h.display(pf::P_O2_ROUTE) == "F1");
    // Everything at once stays finite.
    h.set(pf::P_O1_UNI, 8);
    h.set(pf::P_O1_SUB_LEVEL, 1.0f);
    h.set(pf::P_NOISE_LEVEL, 1.0f);
    h.set(pf::P_O2_WAVE, pf::OW_SQUARE);
    for (int n : {24, 60, 96}) h.on(n, 127);
    CHECK(h.run(40) < 200.0f && h.finite);
}

// The frame cache (the tables are 16-bit; a position that holds still for a chunk plays a float
// copy, two frames premixed) sounds like reading the table directly: a position between two frames,
// one gliding there and stopping, and more frames, levels and subs than the cache holds at once
// (slots evicted, the fill budget spent). Cache on and off agree to float rounding.
void frameCache() {
    auto take = [](const pf::Patch& p, bool cache, const std::vector<int>& notes) {
        pf::Synth s;
        s.setFrameCache(cache);
        s.setPatch(p);
        for (int n : notes) s.noteOn(n, 127);
        std::vector<float> out;
        float l[kBlock], r[kBlock];
        for (int b = 0; b < 60; ++b) {
            s.render(l, r, kBlock);
            out.insert(out.end(), l, l + kBlock);
            out.insert(out.end(), r, r + kBlock);
        }
        return out;
    };
    auto agree = [&](const pf::Patch& p, const std::vector<int>& notes) {
        const auto a = take(p, true, notes), b = take(p, false, notes);
        float peak = 0.0f, diff = 0.0f;
        for (size_t i = 0; i < a.size(); ++i) {
            peak = std::max(peak, std::fabs(a[i]));
            diff = std::max(diff, std::fabs(a[i] - b[i]));
        }
        return peak > 0.05f && diff < 2e-5f * peak;
    };
    pf::Patch p = base();
    p.osc[0].pos = 0.37f;   // Classic: between frames 5 and 6
    p.osc[0].unison = 3;
    p.osc[1].level = 0.6f;
    p.osc[1].pos = 0.61f;
    p.osc[1].unison = 2;
    CHECK(agree(p, {36, 60, 84}));
    p.env2Pos = 0.5f;   // gliding up while env 2 rises and falls, then holding at its sustain
    p.env[1] = {0.05f, 0.05f, 0.5f, 0.3f};
    CHECK(agree(p, {36, 60, 84}));
    p.env2Pos = 0.0f;
    p.osc[0].unison = p.osc[1].unison = 4;
    p.osc[0].subLevel = p.osc[1].subLevel = 0.5f;
    p.osc[1].subWave = pf::CW_SAW;
    CHECK(agree(p, {24, 33, 45, 57, 69, 81, 93, 105}));
}

} // namespace

void oscillatorTests() {
    waves();
    subOsc();
    phases();
    routingAndPan();
    noise();
    surfaceTexts();
    frameCache();
}

} // namespace pft
