// Milestone 4: the comb and vowel filters and the Clean / Normal / Dirty engine modes.
#include "host.h"
#include "../dsp/synth.h"

namespace pft {
namespace {

float noteHzFor(int note) { return 440.0f * std::exp2((static_cast<float>(note) - 69.0f) / 12.0f); }

pf::Patch base() {
    pf::Patch p;
    p.osc[0].unison = 1;
    p.osc[0].level = 0.8f;
    p.osc[1].level = 0.0f;
    p.flt[0].type = pf::F_OFF;
    p.flt[1].type = pf::F_OFF;
    p.flt[0].env = 0.0f;
    p.flt[0].key = 0.0f;
    p.env[0] = {0.001f, 0.1f, 1.0f, 0.05f};
    p.velSens = 0.0f;
    p.engine = pf::EN_CLEAN;
    return p;
}

std::vector<float> play(const pf::Patch& p, int note, int blocks, int skip = 40) {
    pf::Synth s;
    s.setPatch(p);
    s.noteOn(note, 127);
    float l[kBlock], r[kBlock];
    for (int b = 0; b < skip; ++b) s.render(l, r, kBlock);
    std::vector<float> out;
    for (int b = 0; b < blocks; ++b) {
        s.render(l, r, kBlock);
        out.insert(out.end(), l, l + kBlock);
    }
    return out;
}

double autocorr(const std::vector<float>& x, int lag) {
    double s = 0.0, e = 0.0;
    for (size_t i = static_cast<size_t>(lag); i < x.size(); ++i) {
        s += static_cast<double>(x[i]) * x[i - static_cast<size_t>(lag)];
        e += static_cast<double>(x[i]) * x[i];
    }
    return e > 0 ? s / e : 0.0;
}

float rms(const std::vector<float>& x) {
    double s = 0.0;
    for (float v : x) s += static_cast<double>(v) * v;
    return static_cast<float>(std::sqrt(s / static_cast<double>(std::max<size_t>(1, x.size()))));
}

float brightness(const std::vector<float>& x) {
    double d = 0.0, s = 0.0;
    for (size_t i = 1; i < x.size(); ++i) {
        d += static_cast<double>(x[i] - x[i - 1]) * (x[i] - x[i - 1]);
        s += static_cast<double>(x[i]) * x[i];
    }
    return s > 0 ? static_cast<float>(d / s) : 0.0f;
}

int rises(const std::vector<float>& x) {
    int n = 0;
    for (size_t i = 1; i < x.size(); ++i) n += (x[i - 1] < 0.0f && x[i] >= 0.0f) ? 1 : 0;
    return n;
}

void combs() {
    // Noise through a comb tuned to 441 Hz (delay 100 samples), full resonance: Comb+ rings at
    // the delay; Comb- inverts every pass, so it repeats at twice the delay.
    for (int type : {pf::F_COMB_PLUS, pf::F_COMB_MINUS}) {
        pf::Patch p = base();
        p.osc[0].wave = pf::OW_NOISE;
        p.osc[0].pos = 0.5f;
        p.flt[0].type = type;
        p.flt[0].cutoffHz = 441.0f;
        p.flt[0].res = 1.0f;
        const auto x = play(p, 60, 40);
        const double c1 = autocorr(x, 100), c2 = autocorr(x, 200);
        if (type == pf::F_COMB_PLUS) CHECK(c1 > 0.6 && c2 > 0.4);
        else CHECK(c1 < -0.6 && c2 > 0.4);
        CHECK(rms(x) > 0.01f && rms(x) < 2.0f);
    }
    // Keytracked at 100%: the comb follows the note (a plucked-string resonance on any key).
    pf::Patch p = base();
    p.osc[0].wave = pf::OW_NOISE;
    p.flt[0].type = pf::F_COMB_PLUS;
    p.flt[0].cutoffHz = noteHzFor(60);
    p.flt[0].key = 1.0f;
    p.flt[0].res = 1.0f;
    const auto x = play(p, 72, 40);   // an octave up: delay = 44100 / 523.25 = 84 samples
    CHECK(autocorr(x, 84) > 0.5);

    // A new note on a voice whose comb lines still hold an earlier note hears none of it (the
    // lines aren't cleared at note start; what this note hasn't written yet reads as silence):
    // sample for sample what a fresh engine plays.
    {
        pf::Patch q = base();
        q.osc[0].wave = pf::OW_SAW;
        q.flt[0].type = pf::F_COMB_PLUS;
        q.flt[0].cutoffHz = 30.0f;   // a 1470-sample delay: the old note is still in the line
        q.flt[0].res = 1.0f;
        q.flt[1].type = pf::F_COMB_MINUS;
        q.flt[1].cutoffHz = 55.0f;
        pf::Synth used, fresh;
        used.setPatch(q);
        fresh.setPatch(q);
        float l[kBlock], r[kBlock], l2[kBlock], r2[kBlock];
        used.noteOn(40, 127);
        for (int b = 0; b < 30; ++b) used.render(l, r, kBlock);
        used.reset();   // the voice goes free with its lines full
        used.noteOn(52, 100);
        fresh.noteOn(52, 100);
        bool same = true;
        for (int b = 0; b < 40; ++b) {
            used.render(l, r, kBlock);
            fresh.render(l2, r2, kBlock);
            for (int i = 0; i < kBlock; ++i) same = same && l[i] == l2[i] && r[i] == r2[i];
        }
        CHECK(same);
    }
}

void vowels() {
    // A saw through the vowel filter: "E" (second formant 1.84 kHz, first 530 Hz) and "I" (2.29
    // kHz) are brighter than "U" (870 Hz, first 300 Hz). Positions: 20 Hz = A, 112 = E,
    // 632 = I, 3.56 k = O, 20 k = U.
    auto at = [](float hz) {
        pf::Patch p = base();
        p.osc[0].wave = pf::OW_SAW;
        p.flt[0].type = pf::F_VOWEL;
        p.flt[0].cutoffHz = hz;
        p.flt[0].res = 0.6f;
        return play(p, 48, 30);
    };
    const auto e = at(112.0f), i = at(632.0f), u = at(19000.0f);
    CHECK(brightness(e) > 2.0f * brightness(u));
    CHECK(brightness(i) > 1.5f * brightness(u));
    CHECK(rms(i) > 0.01f && rms(u) > 0.005f);
}

void engines() {
    // Clean: no drift; every run identical and exactly in tune.
    pf::Patch p = base();
    p.osc[0].wave = pf::OW_SINE;
    const auto a = play(p, 69, kBlocksPerSec * 2), b = play(p, 69, kBlocksPerSec * 2);
    CHECK(a == b);
    const double expect = 440.0 * static_cast<double>(a.size()) / 44100.0;
    CHECK(std::fabs(rises(a) - expect) <= 2.0);
    // Normal: drifts, but only by cents (440 Hz +-2.5 cents is +-0.64 Hz).
    p.engine = pf::EN_NORMAL;
    const auto n = play(p, 69, kBlocksPerSec * 2);
    CHECK(n != a);
    CHECK(std::fabs(rises(n) - expect) <= 3.0);
    // Dirty: a screaming resonance compresses instead of ringing clean.
    pf::Patch q = base();
    q.osc[0].wave = pf::OW_SAW;
    q.flt[0].type = pf::F_LP12;
    q.flt[0].cutoffHz = 880.0f;
    q.flt[0].res = 1.0f;
    q.osc[0].level = 1.0f;
    const float clean = rms(play(q, 45, 40));
    q.engine = pf::EN_DIRTY;
    const auto d = play(q, 45, 40);
    CHECK(rms(d) < clean);
    CHECK(rms(d) > 0.05f * clean);
    bool finite = true;
    for (float v : d) finite = finite && std::isfinite(v);
    CHECK(finite);
}

void surface() {
    Host h;
    CHECK(h.display(pf::P_ENGINE) == "Normal");
    h.set(pf::P_F1_TYPE, pf::F_VOWEL);
    CHECK(h.display(pf::P_F1_TYPE) == "Vowel");
    h.set(pf::P_F2_TYPE, pf::F_COMB_MINUS);
    CHECK(h.display(pf::P_F2_TYPE) == "Comb-");
    h.set(pf::P_ENGINE, pf::EN_DIRTY);
    for (int n : {36, 60, 84}) h.on(n, 127);
    CHECK(h.run(40) < 200.0f && h.finite);
    const std::string state = h.chunk();
    CHECK(state.find("engine=2\n") != std::string::npos && state.find("f1_type=10\n") != std::string::npos);
}

} // namespace

void filterTests() {
    combs();
    vowels();
    engines();
    surface();
}

} // namespace pft
