// Milestone 5: modulation. LFOs (shapes, rates, sync, delay/fade, global), the 12 x 2
// matrix with via and modifiers, envelope stages as targets, the env 2 loop, XY pads.
#include "host.h"
#include "../dsp/synth.h"

namespace pft {
namespace {

pf::Patch base() {
    pf::Patch p;
    p.osc[0].wave = pf::OW_SINE;
    p.osc[0].unison = 1;
    p.osc[0].level = 0.8f;
    p.osc[1].level = 0.0f;
    p.flt[0].type = pf::F_OFF;
    p.flt[1].type = pf::F_OFF;
    p.env[0] = {0.001f, 0.1f, 1.0f, 0.05f};
    p.velSens = 0.0f;
    p.engine = pf::EN_CLEAN;
    return p;
}

void route(pf::Patch& p, int slot, int src, int tgt, float amt, int mod = pf::MM_NONE, float modAmt = 0.0f,
           int via = pf::MS_NONE) {
    pf::ModSlot& s = p.mod[slot];
    s.src = src;
    s.via = via;
    s.mod = mod;
    s.modAmt = modAmt;
    s.tgt[0] = tgt;
    s.amt[0] = amt;
}

struct Rec {
    std::vector<float> L;
    float rms(double from, double to) const {   // seconds
        const size_t a = static_cast<size_t>(from * 44100.0), b = std::min(L.size(), static_cast<size_t>(to * 44100.0));
        double s = 0.0;
        for (size_t i = a; i < b; ++i) s += static_cast<double>(L[i]) * L[i];
        return b > a ? static_cast<float>(std::sqrt(s / static_cast<double>(b - a))) : 0.0f;
    }
};

Rec play(pf::Synth& s, double seconds, int note = 69, int vel = 127) {
    s.noteOn(note, vel);
    Rec r;
    float l[kBlock], rr[kBlock];
    const int blocks = static_cast<int>(seconds * 44100.0 / kBlock);
    for (int b = 0; b < blocks; ++b) {
        s.render(l, rr, kBlock);
        r.L.insert(r.L.end(), l, l + kBlock);
    }
    return r;
}

Rec play(const pf::Patch& p, double seconds, int note = 69, int vel = 127) {
    pf::Synth s;
    s.setPatch(p);
    return play(s, seconds, note, vel);
}

int rises(const std::vector<float>& x, size_t from, size_t to) {
    int n = 0;
    for (size_t i = from + 1; i < std::min(to, x.size()); ++i) n += (x[i - 1] < 0.0f && x[i] >= 0.0f) ? 1 : 0;
    return n;
}

// A square LFO on the volume (amount -1, unipolar): loud, silent, loud... Count the switches.
int switches(const Rec& r, double seconds, double win) {
    int n = 0;
    bool loud = r.rms(0.0, win) > 0.05f;
    for (double t = win; t + win <= seconds; t += win) {
        const bool now = r.rms(t, t + win) > 0.05f;
        n += now != loud ? 1 : 0;
        loud = now;
    }
    return n;
}

void lfos() {
    // A 5 Hz square, unipolar, on the volume: 10 switches a second.
    pf::Patch p = base();
    p.lfo[0].wave = pf::LW_SQUARE;
    p.lfo[0].rateHz = 5.0f;
    p.lfo[0].unipolar = true;
    route(p, 0, pf::MS_LFO1, pf::MT_VOLUME, -1.0f);
    const Rec r = play(p, 2.0);
    const int sw = switches(r, 2.0, 0.01);
    CHECK(sw >= 18 && sw <= 21);
    // Synced: 1/4 at 120 BPM = 2 Hz -> 4 switches a second.
    p.lfo[0].sync = true;
    p.lfo[0].div = 6;
    const int sync = switches(play(p, 2.0), 2.0, 0.01);
    CHECK(sync >= 7 && sync <= 9);
    // Delay: nothing for the first 0.5 s, then it moves.
    p.lfo[0].sync = false;
    p.lfo[0].delay = 0.5f;
    const Rec d = play(p, 1.5);
    CHECK(switches(d, 0.45, 0.01) == 0);
    CHECK(switches(d, 1.5, 0.01) >= 8);
    // Every shape is finite and in range (pitch vibrato, +-1 st).
    for (int w = 0; w < pf::LW_COUNT; ++w) {
        pf::Patch q = base();
        q.lfo[1].wave = w;
        q.lfo[1].rateHz = 7.0f;
        route(q, 3, pf::MS_LFO2, pf::MT_PITCH, 0.15f);
        const Rec v = play(q, 0.5);
        bool finite = true;
        for (float x : v.L) finite = finite && std::isfinite(x);
        CHECK(finite && v.rms(0.1, 0.5) > 0.02f);
    }
}

void globalLfo() {
    // A Global, synced LFO follows the song position: at beat 0.5 of a 1-bar square it is in
    // its second half whatever the note started.
    pf::Patch p = base();
    p.lfo[0].wave = pf::LW_SQUARE;
    p.lfo[0].sync = true;
    p.lfo[0].div = 3;   // 1 bar = 4 beats
    p.lfo[0].trig = pf::LT_GLOBAL;
    p.lfo[0].unipolar = true;
    route(p, 0, pf::MS_LFO1, pf::MT_VOLUME, -1.0f);
    pf::Synth s;
    s.setPatch(p);
    s.setTransport(120.0, 2.5, true, true);   // second half of the bar: square low -> loud
    const Rec a = play(s, 0.2);
    CHECK(a.rms(0.05, 0.2) > 0.05f);
    pf::Synth t;
    t.setPatch(p);
    t.setTransport(120.0, 0.5, true, true);   // first half: square high -> silent
    const Rec b = play(t, 0.2);
    CHECK(b.rms(0.05, 0.2) < 0.01f);
}

void targetsAndModifiers() {
    // Constant -> pitch at 0.5 = +12 st: A4 sounds A5.
    {
        pf::Patch p = base();
        route(p, 0, pf::MS_CONSTANT, pf::MT_PITCH, 0.5f);
        const Rec r = play(p, 1.0);
        CHECK(std::abs(rises(r.L, 4410, r.L.size()) - static_cast<int>(880.0 * 0.9)) <= 3);
    }
    // XY X1 = 0.5 -> volume -1: amp 0.5. Curve +1 makes the source 0.25 (amp 0.75); Quantize to
    // 2 steps makes 0.3 into 0.5.
    auto level = [](int mod, float modAmt, float x1) {
        pf::Patch p = base();
        p.xy[0] = x1;
        route(p, 0, pf::MS_X1, pf::MT_VOLUME, -1.0f, mod, modAmt);
        return play(p, 0.3).rms(0.1, 0.3);
    };
    const float full = level(pf::MM_NONE, 0.0f, 0.0f);
    CHECK(std::fabs(level(pf::MM_NONE, 0.0f, 0.5f) / full - 0.5f) < 0.02f);
    CHECK(std::fabs(level(pf::MM_CURVE, 1.0f, 0.5f) / full - 0.75f) < 0.02f);
    CHECK(std::fabs(level(pf::MM_QUANTIZE, 0.0f, 0.3f) / full - 0.5f) < 0.02f);
    // Rectify: Alternate (-1 on every other note) becomes +1.
    {
        pf::Patch p = base();
        route(p, 0, pf::MS_ALTERNATE, pf::MT_VOLUME, -0.5f, pf::MM_RECTIFY);
        pf::Synth s;
        s.setPatch(p);
        const float a = play(s, 0.2).rms(0.1, 0.2);
        s.reset();
        const float b = play(s, 0.2).rms(0.1, 0.2);
        CHECK(std::fabs(a - b) < 0.01f * full && std::fabs(a / full - 0.5f) < 0.02f);
    }
    // S&H (amount 0 = 0.5 Hz): a fast saw LFO held for 2 s -> steady level.
    {
        pf::Patch p = base();
        p.lfo[0].wave = pf::LW_SAW_UP;
        p.lfo[0].rateHz = 3.3f;
        p.lfo[0].unipolar = true;
        route(p, 0, pf::MS_LFO1, pf::MT_VOLUME, -0.8f, pf::MM_SAMPLE_HOLD, 0.0f);
        const Rec r = play(p, 1.5);
        CHECK(std::fabs(r.rms(0.2, 0.6) - r.rms(1.0, 1.4)) < 0.01f);
    }
    // Slew (amount 1 = 2 s): Gate (1 at once) -> volume -1 fades slowly instead of cutting.
    {
        pf::Patch p = base();
        route(p, 0, pf::MS_GATE, pf::MT_VOLUME, -1.0f, pf::MM_SLEW, 1.0f);
        const Rec r = play(p, 1.0);
        CHECK(r.rms(0.05, 0.15) > 0.6f * full && r.rms(0.8, 1.0) < r.rms(0.05, 0.15));
    }
    // Via: LFO -> volume via the mod wheel: nothing until the wheel moves.
    {
        pf::Patch p = base();
        p.lfo[0].wave = pf::LW_SQUARE;
        p.lfo[0].rateHz = 5.0f;
        p.lfo[0].unipolar = true;
        route(p, 0, pf::MS_LFO1, pf::MT_VOLUME, -1.0f, pf::MM_NONE, 0.0f, pf::MS_MODWHEEL);
        pf::Synth s;
        s.setPatch(p);
        CHECK(switches(play(s, 1.0), 1.0, 0.01) == 0);
        s.controller(1, 127);
        s.reset();
        CHECK(switches(play(s, 1.0), 1.0, 0.01) >= 8);
    }
    // Envelope stage: attack x16 by the matrix: still rising after 30 ms.
    {
        pf::Patch p = base();
        p.env[0].a = 0.01f;
        const Rec fast = play(p, 0.2);
        route(p, 0, pf::MS_CONSTANT, pf::MT_E1_A, 1.0f);
        const Rec slow = play(p, 0.2);
        CHECK(slow.rms(0.02, 0.04) < 0.6f * fast.rms(0.02, 0.04));
    }
    // Env 2 loop on the volume: the level keeps moving while the key is held.
    {
        pf::Patch p = base();
        p.env[1] = {0.05f, 0.05f, 0.0f, 0.1f};
        p.env[1].loop = true;
        route(p, 0, pf::MS_ENV2, pf::MT_VOLUME, -1.0f);
        const Rec r = play(p, 1.0);
        float lo = 1.0f, hi = 0.0f;
        for (double t = 0.5; t < 1.0; t += 0.02) {
            lo = std::min(lo, r.rms(t, t + 0.02));
            hi = std::max(hi, r.rms(t, t + 0.02));
        }
        CHECK(hi > 2.0f * lo);
    }
}

void plugin() {
    Host h;
    h.set(pf::P_M1_T1, pf::MT_PITCH);
    h.set(pf::P_M1_A1, 0.5f);
    CHECK(h.display(pf::P_M1_A1) == "+12.00 st");
    h.set(pf::P_M1_T1, pf::MT_F1_CUT);
    CHECK(h.display(pf::P_M1_A1) == "+48.0 st");
    h.set(pf::P_M1_T1, pf::MT_E1_A);
    h.set(pf::P_M1_A1, -0.25f);
    CHECK(h.display(pf::P_M1_A1) == "/2.00");
    h.set(pf::P_M1_T1, pf::MT_VOLUME);
    CHECK(h.display(pf::P_M1_A1) == "-25%");
    h.set(pf::P_L1_RATE, 0.5f);
    CHECK(h.display(pf::P_L1_RATE) == "0.50 Hz");
    CHECK(h.display(pf::P_L1_DIV) == "1/4" && h.display(pf::P_M2_SRC) == "None");

    // XY auto-assign fills free slots (slot 1 is in use now).
    h.set(pf::P_M1_SRC, pf::MS_LFO1);
    h.press(pf::P_XY_AUTO);
    CHECK(h.value(pf::P_M2_SRC) == pf::MS_X1 && h.value(pf::P_M2_T1) == pf::MT_F1_CUT);
    CHECK(h.value(pf::P_M3_SRC) == pf::MS_Y1 && h.value(pf::P_M3_T1) == pf::MT_F1_RES);
    CHECK(h.value(pf::P_M9_SRC) == pf::MS_Y4);
    h.run(8);
    CHECK(h.log.automated.count(pf::P_M2_SRC) > 0);   // MPC hears about the new routing
    h.press(pf::P_XY_AUTO);   // again: every axis is used, nothing changes
    CHECK(h.value(pf::P_M10_SRC) == pf::MS_NONE);

    // The mod wheel through MIDI: wheel -> volume -1 silences the note.
    Host w;
    w.bare();
    w.set(pf::P_M1_SRC, pf::MS_MODWHEEL);
    w.set(pf::P_M1_T1, pf::MT_VOLUME);
    w.set(pf::P_M1_A1, -1.0f);
    w.on(69);
    const float before = w.run(20);
    w.midi(0xB0, 1, 127);
    w.run(4);
    const float after = w.run(20);
    CHECK(before > 0.05f && after < 0.01f * before);
    // State: the matrix and the LFOs round-trip.
    Host a;
    a.set(pf::P_M7_SRC, pf::MS_AFTERTOUCH);
    a.set(pf::P_M7_T2, pf::MT_NOISE_COLOR);
    a.set(pf::P_M7_A2, -0.3f);
    a.set(pf::P_L2_WAVE, pf::LW_SMOOTH);
    const std::string state = a.chunk();
    Host b;
    CHECK(b.load(state) == 1);
    CHECK(b.value(pf::P_M7_SRC) == pf::MS_AFTERTOUCH && b.value(pf::P_M7_T2) == pf::MT_NOISE_COLOR);
    CHECK(std::fabs(b.value(pf::P_M7_A2) + 0.3f) < 1e-4f && b.display(pf::P_L2_WAVE) == "Smooth");
    // Everything modulating everything, all 12 slots: finite, bounded.
    Host z;
    for (int k = 0; k < 12; ++k) {
        const int d = k * (pf::P_M2_SRC - pf::P_M1_SRC);
        z.set(pf::P_M1_SRC + d, static_cast<float>(1 + (k * 5) % (pf::MS_COUNT - 1)));
        z.set(pf::P_M1_T1 + d, static_cast<float>(1 + (k * 7) % (pf::MT_COUNT - 1)));
        z.set(pf::P_M1_A1 + d, k % 2 ? 1.0f : -1.0f);
        z.set(pf::P_M1_T2 + d, static_cast<float>(1 + (k * 11) % (pf::MT_COUNT - 1)));
        z.set(pf::P_M1_A2 + d, 0.7f);
        z.set(pf::P_M1_MOD + d, static_cast<float>(k % pf::MM_COUNT));
    }
    for (int n : {30, 60, 90}) z.on(n, 127);
    z.midi(0xD0, 100, 0);
    z.midi(0xB0, 1, 90);
    CHECK(z.run(80) < 200.0f && z.finite);
}

// The matrix runs once per kChunk samples; what it moves glides across the chunk instead of
// stepping there. A 33 Hz sine under a 40 Hz LFO on level, volume or pan: no sample-to-sample
// jump beyond about the sine's own slope (0.0047 of its peak; stepping every 16 samples, the
// engine before the glides jumped 0.02 .. 0.045).
void controlRate() {
    for (int tgt : {pf::MT_O1_LEVEL, pf::MT_VOLUME, pf::MT_PAN, pf::MT_O1_PAN}) {
        pf::Patch p = base();
        p.osc[0].level = 0.5f;
        p.lfo[0].rateHz = 40.0f;
        route(p, 0, pf::MS_LFO1, tgt, tgt == pf::MT_VOLUME ? 0.3f : 0.5f);
        const Rec r = play(p, 0.5, 24);
        float peak = 0.0f, jump = 0.0f;
        for (size_t i = 4410; i < r.L.size(); ++i) {
            peak = std::max(peak, std::fabs(r.L[i]));
            jump = std::max(jump, std::fabs(r.L[i] - r.L[i - 1]));
        }
        CHECK(peak > 0.05f);
        CHECK(jump < 0.008f * peak);
    }
}

} // namespace

void modulationTests() {
    lfos();
    globalLfo();
    targetsAndModifiers();
    controlRate();
    plugin();
}

} // namespace pft
