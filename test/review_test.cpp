// Regressions from the full review (after Milestone 7): one check per fixed finding, on the
// engine, the note generator, the plugin glue, the surface and the file handling.
#include "host.h"
#include "../dsp/notegen.h"
#include "../dsp/synth.h"
#include "../dsp/tuning.h"
#include "../dsp/wavetable.h"
#include "../plugin/library.h"
#include "../plugin/loader.h"
#include "../plugin/paths.h"
#include "../plugin/presets.h"
#include "../plugin/surface.h"

#include <filesystem>
#include <fstream>
#include <limits>
#include <set>

namespace pft {
namespace {
namespace fs = std::filesystem;

// --- engine -----------------------------------------------------------------------------------

pf::Patch plain() {   // one sine, filters off, fast attack, full sustain
    pf::Patch p;
    p.osc[0].pos = 0.0f;
    p.osc[0].unison = 1;
    p.osc[1].level = 0.0f;
    p.flt[0].type = pf::F_OFF;
    p.flt[1].type = pf::F_OFF;
    p.env[0] = {0.001f, 0.1f, 1.0f, 0.05f};
    p.velSens = 0.0f;
    return p;
}

struct Eng {
    pf::Synth s;
    float L[kBlock], R[kBlock];
    explicit Eng(const pf::Patch& p) { s.setPatch(p); }
    float run(int blocks) {
        float peak = 0.0f;
        for (int b = 0; b < blocks; ++b) {
            s.render(L, R, kBlock);
            for (int i = 0; i < kBlock; ++i) peak = std::max(peak, std::fabs(L[i]));
        }
        return peak;
    }
    std::multiset<int> gated() const {
        std::multiset<int> out;
        for (int i = 0; i < pf::kMaxVoices; ++i) {
            const auto v = s.voiceInfo(i);
            if (v.active && v.gate) out.insert(v.note);
        }
        return out;
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
        return -1000.0f;
    }
};

float rms(const float* x, int n) {
    double e = 0.0;
    for (int i = 0; i < n; ++i) e += static_cast<double>(x[i]) * x[i];
    return static_cast<float>(std::sqrt(e / n));
}

void engineFixes() {
    // A chord into a full pool keeps every note (not only its last), in every steal mode.
    for (int mode : {pf::ST_OLDEST, pf::ST_QUIETEST, pf::ST_KEEP_LOW, pf::ST_KEEP_HIGH}) {
        pf::Patch p = plain();
        p.voices = 4;
        p.steal = mode;
        Eng e(p);
        for (int n : {48, 52, 55, 59}) { e.s.noteOn(n, 100); e.run(2); }
        e.s.noteOn(70, 100);
        e.s.noteOn(72, 100);   // same sample
        e.run(4);
        const auto g = e.gated();
        CHECK(g.count(70) == 1 && g.count(72) == 1);
    }
    // Envelope knobs still work once the matrix modulates a stage (velocity -> attack).
    {
        pf::Patch p = plain();
        p.mod[0].src = pf::MS_VELOCITY;
        p.mod[0].tgt[0] = pf::MT_E1_A;
        p.mod[0].amt[0] = 0.25f;
        Eng e(p);
        e.s.noteOn(60, 100);
        e.run(10);
        e.s.noteOff(60);
        e.run(kBlocksPerSec);
        p.env[0].a = 5.0f;   // a slow attack now
        e.s.setPatch(p);
        e.s.noteOn(60, 100);
        e.run(7);            // ~20 ms
        CHECK(e.s.voiceInfo(0).level < 0.05f);
    }
    // Comb into Vowel: the comb still feeds back (it rings on after its input stops).
    {
        pf::Patch p = plain();
        p.flt[0].type = pf::F_COMB_PLUS;
        p.flt[0].res = 1.0f;        // feedback 0.97
        p.flt[0].key = 0.0f;
        p.flt[0].env = 0.0f;
        p.flt[0].cutoffHz = 110.0f; // a 400-sample delay
        p.flt[1].type = pf::F_VOWEL;
        p.flt[1].res = 0.0f;
        p.flt[1].cutoffHz = 2000.0f;
        p.env[0].r = 3.0f;
        Eng e(p);
        e.s.noteOn(57, 127);
        e.run(20);
        const float on = rms(e.L, kBlock);
        p.osc[0].level = 0.0f;   // the source goes quiet
        e.s.setPatch(p);
        e.run(34);               // ~100 ms
        CHECK(rms(e.L, kBlock) > 0.2f * on);
    }
    // A key that goes up during the 3 ms steal fade still plays (and the pedal holds it).
    for (bool pedal : {false, true}) {
        pf::Patch p = plain();
        p.voices = 4;
        Eng e(p);
        for (int n : {48, 52, 55, 59}) { e.s.noteOn(n, 100); e.run(2); }
        e.s.sustain(pedal);
        e.s.noteOn(80, 100);
        e.s.noteOff(80);
        e.run(4);
        CHECK(e.sounds(80));
        if (pedal) CHECK(e.gated().count(80) == 0);   // up, held by the pedal
        e.run(kBlocksPerSec / 4);
        CHECK(e.sounds(80) == pedal);
    }
    // The same note again during a fade doesn't cancel the fade (no jump back to full level).
    {
        pf::Patch p = plain();
        p.voices = 2;
        Eng e(p);
        e.s.noteOn(48, 127);
        e.s.noteOn(52, 127);
        e.run(20);
        e.s.noteOn(70, 127);   // steals 48: fading
        e.s.noteOn(70, 127);   // again, inside the fade
        float last = e.L[kBlock - 1], jump = 0.0f;
        for (int b = 0; b < 3; ++b) {
            e.s.render(e.L, e.R, kBlock);
            for (int i = 0; i < kBlock; ++i) {
                jump = std::max(jump, std::fabs(e.L[i] - last));
                last = e.L[i];
            }
        }
        CHECK(jump < 0.05f);
        CHECK(e.gated().count(70) == 1);
    }
    // Mono with Legato glide: going back to a held key glides too.
    {
        pf::Patch p = plain();
        p.voiceMode = pf::VM_MONO;
        p.glideMode = pf::GL_LEGATO;
        p.glideTime = 0.2f;
        Eng e(p);
        e.s.noteOn(60, 100);
        e.run(4);
        e.s.noteOn(72, 100);
        e.run(kBlocksPerSec);
        e.s.noteOff(72);
        e.run(kBlocksPerSec / 10);   // half the glide
        const float at = e.s.voiceInfo(0).pitch;
        CHECK(at > 62.0f && at < 70.0f);
    }
    // Mono with the pedal: the key is up (gate off), the note is held.
    {
        pf::Patch p = plain();
        p.voiceMode = pf::VM_MONO;
        Eng e(p);
        e.s.sustain(true);
        e.s.noteOn(60, 100);
        e.run(2);
        e.s.noteOff(60);
        e.run(2);
        CHECK(!e.s.voiceInfo(0).gate && e.s.voiceInfo(0).active);
    }
    // A slow glide over a tiny (microtonal) step arrives on time instead of stalling.
    {
        static float tuning[128];
        for (int n = 0; n < 128; ++n) tuning[n] = static_cast<float>(n);
        tuning[60] = 100.0f;
        tuning[61] = 100.08f;
        pf::Patch p = plain();
        p.tuning = tuning;
        p.voiceMode = pf::VM_MONO;
        p.glideMode = pf::GL_ALWAYS;
        p.glideTime = 4.0f;
        Eng e(p);
        e.s.noteOn(60, 100);
        e.run(4);
        e.s.noteOn(61, 100);
        e.run(kBlocksPerSec * 2);   // half way
        CHECK(std::fabs(e.s.voiceInfo(0).pitch - 100.04f) < 0.005f);
        e.run(kBlocksPerSec * 3);
        CHECK(e.s.voiceInfo(0).pitch == 100.08f);
    }
    // Tunings with negative pitches still glide from the last note (no "-1 = none" marker).
    {
        static float low[128];
        for (int n = 0; n < 128; ++n) low[n] = static_cast<float>(n) - 80.0f;
        pf::Patch p = plain();
        p.tuning = low;
        p.glideMode = pf::GL_ALWAYS;
        p.glideTime = 0.5f;
        Eng e(p);
        e.s.noteOn(40, 100);
        e.run(4);
        e.s.noteOn(52, 100);
        e.run(kBlocksPerSec / 4);   // half the glide
        const float at = e.pitchOf(52);
        CHECK(at > -39.0f && at < -29.0f);   // on its way from -40 to -28
    }
    // Duo: a voice fading out to start a held key counts as sounding it (no duplicate).
    {
        pf::Patch p = plain();
        p.voiceMode = pf::VM_DUO;
        Eng e(p);
        e.s.noteOn(48, 100);
        e.s.noteOn(52, 100);
        e.run(10);
        e.s.noteOn(55, 100);   // steals a voice: fading, will play 55
        e.s.noteOff(52);       // a key goes up inside the fade
        e.run(10);
        const auto g = e.gated();
        CHECK(g.count(55) == 1);
    }
    // Phase 360 degrees is phase 0 (no out-of-range conversion; UBSan watches).
    {
        pf::Patch p = plain();
        p.osc[0].phase = 1.0f;
        p.osc[0].subLevel = 0.5f;
        Eng e(p);
        e.s.noteOn(60, 100);
        CHECK(e.run(4) > 0.0f);
    }
    // Drive fades in: no level step just above 0.
    {
        float level[2];
        for (int k = 0; k < 2; ++k) {
            pf::Patch p = plain();
            p.osc[0].level = 0.1f;
            p.flt[0].type = pf::F_LP12;
            p.flt[0].cutoffHz = 18000.0f;
            p.flt[0].env = 0.0f;
            p.flt[0].drive = k ? 0.001f : 0.0f;
            Eng e(p);
            e.s.noteOn(60, 100);
            e.run(20);
            level[k] = rms(e.L, kBlock);
        }
        CHECK(std::fabs(level[1] / level[0] - 1.0f) < 0.03f);
    }
    // Noise: the colour changes the tone, not the level, at the dark end too.
    {
        float level[2];
        for (int k = 0; k < 2; ++k) {
            pf::Patch p = plain();
            p.osc[0].level = 0.0f;
            p.noise.level = 0.5f;
            p.noise.color = k ? -1.0f : 0.0f;
            Eng e(p);
            e.s.noteOn(60, 100);
            e.run(10);
            double acc = 0.0;
            for (int b = 0; b < 40; ++b) {
                e.s.render(e.L, e.R, kBlock);
                for (int i = 0; i < kBlock; ++i) acc += static_cast<double>(e.L[i]) * e.L[i];
            }
            level[k] = static_cast<float>(std::sqrt(acc / (40.0 * kBlock)));
        }
        CHECK(std::fabs(20.0f * std::log10(level[1] / level[0])) < 1.5f);
    }
    // Smoothing doesn't depend on how render() is split (MIDI events split chunks).
    {
        float out[2][64];
        for (int k = 0; k < 2; ++k) {
            pf::Patch p = plain();
            p.volumeDb = -60.0f;
            Eng e(p);
            e.s.noteOn(60, 100);
            e.run(4);
            p.volumeDb = 0.0f;
            e.s.setPatch(p);
            float L[64], R[64];
            if (k == 0) e.s.render(L, R, 64);
            else
                for (int i = 0; i < 64; ++i) e.s.render(L + i, R + i, 1);
            std::copy(L, L + 64, out[k]);
        }
        CHECK(std::fabs(rms(out[1], 64) / std::max(rms(out[0], 64), 1e-9f) - 1.0f) < 0.25f);
    }
    // .tun: [Tuning] counts from the fixed 8.18 Hz, [Exact Tuning] from BaseFreq.
    {
        pf::Tuning t;
        CHECK(pf::parseTun("[Tuning]\nnote 60=6000\n[Exact Tuning]\nBaseFreq=16.3515978313\nnote 69=6900\n", t));
        CHECK(std::fabs(t.pitch[60] - 60.0f) < 1e-3f);
        CHECK(std::fabs(t.pitch[69] - 81.0f) < 1e-3f);
    }
}

// --- note generator ---------------------------------------------------------------------------

struct Gen {
    pf::Synth synth;
    pf::NoteGen gen;
    pf::SeqPatch seq;
    double beats = 0.0;
    bool playing = false;
    std::vector<int> ons;
    std::multiset<int> gated;

    Gen() {
        pf::Patch p;
        p.env[0] = {0.001f, 0.1f, 1.0f, 0.01f};
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
            if (g.count(n) > gated.count(n)) ons.push_back(n);
        gated = g;
    }
    void block() {
        gen.setTransport(120.0, beats, playing, playing);
        gen.setPatch(seq, synth);
        gen.advance(0, synth);
        scan();
        float L[kBlock], R[kBlock];
        int pos = 0;
        while (pos < kBlock) {
            const int step = gen.untilNext(kBlock - pos);
            synth.render(L + pos, R + pos, step);
            pos += step;
            gen.advance(step, synth);
            scan();
        }
        if (playing) beats += kBlock / 44100.0 * 2.0;
    }
    void blocks(double beatsLong) {
        for (int b = 0; b < static_cast<int>(beatsLong * 44100.0 / 2.0 / kBlock); ++b) block();
    }
};

void noteGenFixes() {
    // A new phrase on the free-running clock: the last note of the old one ends on time.
    {
        Gen g;
        g.seq.gate = 0.9f;
        g.block();
        g.gen.keyOn(60, 100, g.synth);
        g.blocks(10.1);
        g.gen.keyOff(60, g.synth);
        g.gen.keyOn(64, 100, g.synth);   // the clock restarts at 0
        g.blocks(0.5);
        CHECK(g.gated.count(60) == 0);
    }
    // Rate changes re-aim the grid: no silence, no burst.
    {
        Gen g;
        g.playing = true;
        g.block();
        g.gen.keyOn(60, 100, g.synth);
        g.blocks(60.0);
        g.seq.rate = 6;   // 1/4
        g.ons.clear();
        g.blocks(1.2);
        CHECK(!g.ons.empty());
        g.seq.rate = 15;   // 1/32: the step at this beat, not hundreds at once
        g.ons.clear();
        g.block();
        CHECK(g.ons.size() <= 1);
    }
    // A loop back: the note sounding at the loop point ends on time.
    {
        Gen g;
        g.playing = true;
        g.seq.rate = 6;   // 1/4
        g.seq.gate = 0.9f;
        g.block();
        g.gen.keyOn(60, 100, g.synth);
        g.gen.keyOn(64, 100, g.synth);
        g.blocks(3.5);    // the step at beat 3 plays until 3.9
        CHECK(g.gated.size() == 1);
        const int sounding = g.gated.empty() ? -1 : *g.gated.begin();
        g.beats = 0.0;    // the loop
        g.blocks(0.6);
        CHECK(g.gated.count(sounding) == 0);   // ended at 0.4, not at 3.9 of the next pass
    }
    // Notes heard while recording end when recording stops before the key goes up.
    {
        Gen g;
        g.seq.mode = pf::SQ_SEQ;
        g.seq.record = true;
        g.block();
        g.gen.keyOn(62, 100, g.synth);
        g.block();
        CHECK(g.gated.count(62) == 1);
        g.seq.record = false;
        g.block();
        CHECK(g.gated.count(62) == 0);
        g.gen.keyOff(62, g.synth);
    }
    // A count-in (negative song position): the shape lanes stay in range (UBSan watches).
    {
        Gen g;
        g.seq.shape[0][7] = 0.5f;
        g.playing = true;
        g.beats = -2.0;
        g.block();
        float out[4];
        g.gen.shapeValues(out);
        CHECK(std::isfinite(out[0]) && out[0] >= -1.0f && out[0] <= 1.0f);
    }
    // The Seq lane moves at the step itself, not a block later.
    {
        Gen g;
        g.seq.mode = pf::SQ_SEQ;
        for (int k = 0; k < pf::kSeqSteps; ++k) g.seq.mod[k] = k % 2 ? 1.0f : -1.0f;
        g.block();
        g.gen.keyOn(60, 100, g.synth);
        int checked = 0;
        for (int b = 0; b < 300; ++b) {
            g.gen.setTransport(120.0, 0.0, false, false);
            g.gen.setPatch(g.seq, g.synth);
            g.gen.advance(0, g.synth);
            float L[kBlock], R[kBlock];
            for (int pos = 0; pos < kBlock;) {
                const int step = g.gen.untilNext(kBlock - pos);
                g.synth.render(L + pos, R + pos, step);
                pos += step;
                const int was = g.gen.currentStep();
                g.gen.advance(step, g.synth);
                if (g.gen.currentStep() != was) {
                    CHECK(g.gen.seqValue() == g.seq.mod[g.gen.currentStep()]);
                    ++checked;
                }
            }
        }
        CHECK(checked >= 5);
    }
    // Latch: a key pressed twice is one note of the chord; a partly released chord stays whole.
    {
        Gen g;
        g.seq.latch = true;
        g.block();
        g.gen.keyOn(60, 100, g.synth);
        g.gen.keyOn(64, 100, g.synth);
        g.gen.keyOff(60, g.synth);
        g.gen.keyOn(60, 100, g.synth);   // again, while 64 is down
        g.gen.keyOn(67, 100, g.synth);
        g.gen.keyOff(67, g.synth);       // part of the chord goes up
        g.ons.clear();
        g.blocks(2.0);                    // 8 steps of 1/16
        CHECK(g.ons.size() >= 6);
        const std::vector<int> want = {60, 64, 67, 60, 64, 67};
        CHECK(std::vector<int>(g.ons.begin(), g.ons.begin() + 6) == want);
    }
    // The pedal in Arp mode holds the keys (the arp goes on), not every generated note.
    {
        Gen g;
        g.block();
        g.gen.keyOn(60, 100, g.synth);
        g.gen.pedal(true, g.synth);
        g.gen.keyOff(60, g.synth);
        g.ons.clear();
        g.blocks(1.0);
        CHECK(g.ons.size() >= 3);    // still arpeggiating
        g.gen.pedal(false, g.synth);
        g.blocks(0.5);
        g.ons.clear();
        g.blocks(1.0);
        CHECK(g.ons.empty());        // stopped
        CHECK(g.synth.activeVoices() == 0);   // nothing left sustained
    }
    // A NaN song position from the host neither hangs nor breaks the sound.
    {
        Host h;
        h.set(pf::P_SEQ_MODE, pf::SQ_ARP);
        h.log.time.flags |= vst::kVstTransportPlaying;
        h.log.time.ppqPos = std::numeric_limits<double>::quiet_NaN();
        h.on(60);
        h.run(20);
        h.log.time.tempo = std::numeric_limits<double>::infinity();
        h.run(20);
        CHECK(h.finite);
    }
}

// --- plugin and surface -----------------------------------------------------------------------

void surfaceFixes() {
    // Value texts never round into the next unit.
    CHECK(pf::paramDisplay(pf::P_F1_CUT, pf::paramNorm(pf::P_F1_CUT, 999.7f)) == "1.00 kHz");
    CHECK(pf::paramDisplay(pf::P_F1_CUT, pf::paramNorm(pf::P_F1_CUT, 9996.0f)) == "10.0 kHz");
    CHECK(pf::paramDisplay(pf::P_E1_A, pf::paramNorm(pf::P_E1_A, 0.9997f)) == "1.00 s");
    CHECK(pf::paramDisplay(pf::P_E1_A, pf::paramNorm(pf::P_E1_A, 0.00996f)) == "10 ms");
    CHECK(pf::paramDisplay(pf::P_O1_FINE, pf::paramNorm(pf::P_O1_FINE, -0.4f)) == "0 ct");
    CHECK(pf::paramDisplay(pf::P_VOLUME, pf::paramNorm(pf::P_VOLUME, -0.01f)) == "0.0 dB");
    CHECK(pf::paramDisplay(pf::P_O1_PAN, pf::paramNorm(pf::P_O1_PAN, -0.004f)) == "C");

    // State text: a bad header is refused, a byte-order mark is fine, and a project that
    // doesn't name the tables leaves them alone.
    {
        Host h;
        CHECK(h.load("polyforce x\nf1_cut=500\n") == 0);
        CHECK(h.load("polyforce \n") == 0);
        CHECK(h.load("\xEF\xBB\xBFpolyforce 4\nf1_cut=500\n") == 1 && h.display(pf::P_F1_CUT) == "500 Hz");
        CHECK(h.load("polyforce 4\no1_table=builtin:PWM\n") == 1);
        CHECK(h.until([&] { return h.display(pf::P_O1_TABLE) == "Built-in / PWM"; }));
        CHECK(h.load("polyforce 4\nf1_cut=600\n") == 1);
        h.run(4);
        CHECK(h.display(pf::P_O1_TABLE) == "Built-in / PWM");
    }
    // A text that follows another parameter is announced when that one changes.
    {
        Host h;
        h.run(8);
        const int before = h.log.updates;
        h.set(pf::P_O1_WAVE, pf::OW_PULSE);
        h.run(8);
        CHECK(h.log.updates > before);
        CHECK(h.display(pf::P_O1_POS).compare(0, 5, "WIDTH") == 0);
    }
    // An open popup list closes when its page goes away.
    {
        Host h;
        h.setN(pf::P_O1_WAVE__OPEN, 1.0f);
        CHECK(h.get(pf::P_O1_WAVE__OPEN) == 1.0f);
        h.set(pf::P_UI_OSC, 1);   // OSC 2
        CHECK(h.get(pf::P_O1_WAVE__OPEN) == 0.0f);
    }
    // A preset picked from FAVORITES: the browser stays on FAVORITES.
    {
        pf::presetLibrary().setFavorite("builtin:Acid Bass", true);
        Host h;
        h.setN(pf::P_BR_TARGET, 1.0f);
        h.run(2);
        h.setN(pf::P_CAT_1, 1.0f);   // FAVORITES
        CHECK(h.get(pf::P_CAT_1) == 1.0f && h.display(pf::P_TBL_1) == "Acid Bass");
        h.setN(pf::P_TBL_1, 1.0f);
        CHECK(h.display(pf::P_PRESET) == "PRESET  Factory / Acid Bass");
        CHECK(h.get(pf::P_CAT_1) == 1.0f);
        pf::presetLibrary().setFavorite("builtin:Acid Bass", false);
    }
    // A stepper on an unlisted item (after RANDOM) moves from where it stood, not to item 1.
    {
        Host h;
        const auto L = pf::presetLibrary().listing();
        for (int k = 0; k < 12; ++k) h.press(pf::P_PRESET_NEXT);   // the first press picks item 0
        CHECK(h.display(pf::P_PRESET) == "PRESET  Factory / " + L->items[11].name);
        h.press(pf::P_PRE_RAND);
        CHECK(h.display(pf::P_PRESET) == "PRESET  -");
        h.setN(pf::P_PRESET, h.get(pf::P_PRESET) - 1.0f / 128.0f);   // one detent left
        CHECK(h.display(pf::P_PRESET) == "PRESET  Factory / " + L->items[10].name);
    }
    // RANDOM: the main filter stays a filter, the sub oscillator's wave is left alone.
    {
        Host h;
        h.set(pf::P_RAND_AMT, 1.0f);
        bool filterOff = false, subChanged = false;
        for (int k = 0; k < 60; ++k) {
            h.press(pf::P_PRE_RAND);
            filterOff = filterOff || h.value(pf::P_F1_TYPE) == pf::F_OFF;
            subChanged = subChanged || h.value(pf::P_O1_SUB_WAVE) != 0.0f;
        }
        CHECK(!filterOff && !subChanged);
    }
    // XY auto-assign claims a slot clean: no leftover via or second target.
    {
        Host h;
        h.set(pf::P_M1_VIA, pf::MS_LFO1);
        h.set(pf::P_M1_T2, pf::MT_PITCH);
        h.set(pf::P_M1_A2, 1.0f);
        h.press(pf::P_XY_AUTO);
        CHECK(h.value(pf::P_M1_SRC) == pf::MS_X1);
        CHECK(h.value(pf::P_M1_VIA) == pf::MS_NONE && h.value(pf::P_M1_T2) == pf::MT_OFF && h.value(pf::P_M1_A2) == 0.0f);
    }
    // A batch (preset, project, randomize) is never seen half written by the audio thread.
    {
        pf::Loader loader{{pf::tableSlotType(), pf::tableSlotType(), pf::tuningSlotType()}};
        pf::Surface s(loader);
        float out[pf::P_COUNT];
        CHECK(s.snapshot(out));
        s.beginBatch();
        s.beginBatch();   // nested
        s.endBatch();
        CHECK(!s.snapshot(out));
        s.endBatch();
        CHECK(s.snapshot(out));
        loader.stop();
    }
}

// --- files ------------------------------------------------------------------------------------

void fileFixes() {
    // Keys: names with dots are fine, ways out of the root are not.
    const std::vector<pf::Root> roots = {{"plugin", "/x"}};
    CHECK(pf::resolveKey("plugin:Leads/Lead...wav", roots) == "/x/Leads/Lead...wav");
    CHECK(pf::resolveKey("plugin:../etc/passwd", roots).empty());
    CHECK(pf::resolveKey("plugin:a/../../b.wav", roots).empty());
    CHECK(pf::resolveKey("plugin:/etc/passwd", roots).empty());

    // A huge file named *.wav fails like a broken one: no crash, no 100 MB read.
    {
        const std::string dir = fixtureDir() + "/plugin/Big";
        fs::create_directories(dir);
        { std::ofstream(dir + "/huge.wav") << "RIFF"; }
        fs::resize_file(dir + "/huge.wav", 100u << 20);   // sparse
        pf::tableLibrary().rescan();
        Host h;
        CHECK(h.load("polyforce 4\no1_table=plugin:Big/huge.wav\n") == 1);
        CHECK(h.until([&] { return h.display(pf::P_O1_TABLE) == "MISSING Big / huge"; }));
        h.on(60);
        CHECK(h.run(10) > 1e-3f);   // the fallback plays
        fs::remove_all(dir);
        pf::tableLibrary().rescan();
    }
    // A tuning load never lands in the wavetables' Recent list.
    {
        Host h;
        h.press(pf::P_TUNING_NEXT);
        CHECK(h.until([&] { return h.display(pf::P_TUNING).compare(0, 8, "TUNING  ") == 0 &&
                                   h.display(pf::P_TUNING) != "TUNING  Built-in / 12-TET"; }));
        std::string recent;
        pf::readFile(fixtureDir() + "/data/recent.txt", recent);
        CHECK(recent.find(".scl") == std::string::npos && recent.find(".tun") == std::string::npos);
    }
    // User presets: a deleted number is never reused.
    {
        Host h;
        h.press(pf::P_PRE_SAVE);
        h.press(pf::P_PRE_SAVE);
        int top = 0;
        for (const auto& e : fs::directory_iterator(fixtureDir() + "/presets/User")) {
            int n = 0;
            if (std::sscanf(e.path().filename().string().c_str(), "User %d.pfp", &n) == 1) top = std::max(top, n);
        }
        CHECK(top >= 2);
        char name[64];
        std::snprintf(name, sizeof name, "/presets/User/User %03d.pfp", top - 1);
        fs::remove(fixtureDir() + name);
        h.press(pf::P_PRE_SAVE);
        std::snprintf(name, sizeof name, "/presets/User/User %03d.pfp", top + 1);
        CHECK(fs::exists(fixtureDir() + name));
        std::snprintf(name, sizeof name, "/presets/User/User %03d.pfp", top - 1);
        CHECK(!fs::exists(fixtureDir() + name));
    }
    // Replaced objects are freed while no block runs, and never while one does.
    {
        std::atomic<int> freed{0};
        pf::Loader::SlotType t;
        t.fallbackKey = "a";
        t.load = [&freed](const std::string&, std::string*, int* info) -> std::shared_ptr<const void> {
            if (info) *info = 1;
            return std::shared_ptr<const int>(new int(1), [&freed](const int* p) {
                delete p;
                ++freed;
            });
        };
        pf::Loader L({t});
        auto settle = [&](int want) {
            for (int i = 0; i < 200 && freed.load() < want; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return freed.load();
        };
        L.want(0, "x", true);
        CHECK(settle(1) == 1);   // "a" went, though no block ever ran
        L.blockStart();
        L.want(0, "y", true);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(freed.load() == 1);   // "x" may be in use by the running block
        L.blockDone();
        CHECK(settle(2) == 2);
        L.stop();
    }
}

// --- second review (after the performance pass) ---------------------------------------------

void noteGenFixes2() {
    // A large host song position: step indices were a 32-bit long on the Force and the step
    // search never ended (from ~5.4e8 quarter notes at 1/16). test-arm runs this on ARM.
    {
        Gen g;
        g.playing = true;
        g.beats = 6e8;
        g.block();
        g.gen.keyOn(60, 100, g.synth);
        g.ons.clear();
        g.blocks(1.0);   // four 1/16 steps
        CHECK(g.ons.size() >= 3);
    }
    // Swing turned up right after an odd step played moves that step later, past the clock:
    // it doesn't play a second time.
    {
        Gen g;
        g.seq.gate = 0.05f;
        g.block();
        g.gen.keyOn(60, 100, g.synth);              // step 0 next sample, step 1 ~5512 samples on
        for (int b = 0; b < 47; ++b) g.block();     // step 1 played and ended
        const size_t played = g.ons.size();
        CHECK(played == 2);
        g.seq.swing = 0.5f;                         // step 1 would now be at ~8270
        for (int b = 0; b < 35; ++b) g.block();     // up to just before step 2
        CHECK(g.ons.size() == played);
    }
    // Play pressed right after a free-running step fired at a block's end, from a grid line:
    // the sequence starts at step 0 (it was taken for a loop's end and skipped).
    {
        Gen g;
        g.seq.mode = pf::SQ_SEQ;
        g.seq.rate = 6;   // 1/4: 22050 samples
        g.seq.steps = 4;
        for (int k = 0; k < 4; ++k) g.seq.note[k] = k;
        g.block();
        // The key 94 samples into a block: step 1 falls exactly on a block boundary (22272).
        g.gen.setTransport(120.0, 0.0, false, false);
        g.gen.setPatch(g.seq, g.synth);
        g.gen.advance(0, g.synth);
        float L[kBlock], R[kBlock];
        for (int pos = 0; pos < kBlock;) {
            const int step = pos < 94 ? g.gen.untilNext(94 - pos) : g.gen.untilNext(kBlock - pos);
            g.synth.render(L + pos, R + pos, step);
            pos += step;
            g.gen.advance(step, g.synth);
            if (pos == 94) g.gen.keyOn(60, 100, g.synth);
        }
        while (g.ons.size() < 2) g.block();         // step 1 (61) fired as the block ended
        CHECK(g.ons.back() == 61);
        g.ons.clear();
        g.playing = true;
        g.beats = 0.0;
        g.block();
        CHECK(!g.ons.empty() && g.ons.front() == 60);
    }
    // A key pressed again inside its last note's gate: the new note isn't cut short by the
    // old one's pending end.
    {
        Gen g;
        g.seq.rate = 6;   // 1/4
        g.seq.gate = 0.9f;
        g.block();
        g.gen.keyOn(60, 100, g.synth);
        for (int b = 0; b < 34; ++b) g.block();
        g.gen.keyOff(60, g.synth);
        g.gen.keyOn(60, 100, g.synth);              // a new phrase: its step 0 plays 0.9 beat
        for (int b = 0; b < 137; ++b) g.block();    // past where the old note ended (~19970)
        CHECK(g.gated.count(60) == 1);
    }
    // A new latched chord starts from its first note.
    {
        Gen g;
        g.seq.latch = true;
        g.block();
        for (int n : {60, 64, 67}) g.gen.keyOn(n, 100, g.synth);
        for (int n : {60, 64, 67}) g.gen.keyOff(n, g.synth);
        for (int b = 0; b < 70; ++b) g.block();     // two steps: the order stands on the chord's third note
        for (int n : {48, 52, 55}) g.gen.keyOn(n, 100, g.synth);
        for (int n : {48, 52, 55}) g.gen.keyOff(n, g.synth);
        g.ons.clear();
        g.blocks(1.0);
        CHECK(g.ons.size() >= 3 && std::vector<int>(g.ons.begin(), g.ons.begin() + 3) == std::vector<int>({48, 52, 55}));
    }
    // 17 keys: recording, and Off -> Arp. Every key-up still ends its note (the 17th was played
    // but not remembered; a key pushed out of the list kept sounding after the switch).
    for (int steal : {pf::ST_OLDEST, pf::ST_KEEP_LOW}) {
        for (int kind = 0; kind < 2; ++kind) {
            Gen g;
            pf::Patch p;
            p.env[0] = {0.001f, 0.1f, 1.0f, 0.01f};
            p.steal = steal;
            g.synth.setPatch(p);
            g.seq.mode = kind == 0 ? pf::SQ_SEQ : pf::SQ_OFF;
            g.seq.record = kind == 0;
            g.block();
            for (int n = 40; n < 57; ++n) g.gen.keyOn(n, 100, g.synth);
            g.block();
            if (kind == 1) {
                g.seq.mode = pf::SQ_ARP;
                g.block();
            }
            for (int n = 40; n < 57; ++n) g.gen.keyOff(n, g.synth);
            g.blocks(2.0);
            CHECK(g.gated.empty());
        }
    }
    // Recording while MPC plays writes the playing step, not the next step-by-step slot.
    {
        Gen g;
        g.seq.mode = pf::SQ_SEQ;
        g.seq.record = true;
        g.playing = true;
        g.block();
        g.blocks(1.0);
        g.gen.keyOn(62, 100, g.synth);
        pf::NoteGen::Recorded r{};
        CHECK(g.gen.takeRecorded(r));
        CHECK(r.step == g.gen.currentStep() && r.step >= 3);
        g.gen.keyOff(62, g.synth);
    }
}

void pluginFixes2() {
    // A NaN from the host is read as 0: kept, it reached the engine's smoothers (cutoff,
    // volume) and the instance played NaN until it was reloaded.
    {
        Host h;
        h.bare();
        h.set(pf::P_F1_TYPE, pf::F_LP12);
        const float nan = std::numeric_limits<float>::quiet_NaN();
        h.setN(pf::P_F1_CUT, nan);
        h.setN(pf::P_VOLUME, nan);
        h.on(60);
        h.run(10);
        CHECK(std::isfinite(h.get(pf::P_VOLUME)) && h.display(pf::P_VOLUME).find("nan") == std::string::npos);
        h.set(pf::P_VOLUME, -6.0f);
        h.set(pf::P_F1_CUT, 2000.0f);
        h.on(64);
        const float peak = h.run(20);
        CHECK(h.finite && peak > 0.01f);
    }
    // A chunk far bigger than any state is refused, not copied and parsed.
    {
        Host h;
        CHECK(h.load("polyforce 4\n" + std::string(2u << 20, '#')) == 0);
    }
    // A flood of events in one block still has room for the note-off (it used to be dropped
    // with everything past 512 events: a stuck note).
    {
        Host h;
        h.bare();
        h.set(pf::P_E1_R, 0.005f);
        h.on(60);
        h.run(4);
        for (int i = 0; i < 600; ++i) h.midi(0xB0, 1, static_cast<uint8_t>(i & 127));
        h.off(60);
        h.run(10);
        CHECK(h.run(4) < 1e-4f);
    }
    // A float WAV with values near FLT_MAX: refused or loaded finite, never a NaN table.
    {
        const std::string path = fixtureDir() + "/fltmax.wav";
        std::vector<float> smp(2048);   // a square at +-FLT_MAX: band-limiting it overshoots past the float range
        for (int i = 0; i < 2048; ++i) smp[static_cast<size_t>(i)] = (i < 1024 ? 1.0f : -1.0f) * std::numeric_limits<float>::max();
        std::ofstream f(path, std::ios::binary);
        auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
        auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
        f.write("RIFF", 4);
        u32(4 + 24 + 8 + 2048 * 4);
        f.write("WAVEfmt ", 8);
        u32(16);
        u16(3);   // float
        u16(1);
        u32(44100);
        u32(44100 * 4);
        u16(4);
        u16(32);
        f.write("data", 4);
        u32(2048 * 4);
        f.write(reinterpret_cast<const char*>(smp.data()), 2048 * 4);
        f.close();
        pf::Wavetable t;
        std::string err;
        if (pf::loadWavetable(path, t, &err)) {
            bool finite = true;
            for (float v : t.data) finite = finite && std::isfinite(v);
            CHECK(finite);
        }
    }
    // Tuning files: a byte-order mark is fine; garbage numbers are errors, not 0; wide scales
    // (a tritave per degree) load with their far notes clamped.
    {
        const std::string path = fixtureDir() + "/bom.tun";
        std::ofstream(path) << "\xEF\xBB\xBF[Tuning]\nnote 69=6950\n";
        pf::Tuning t;
        CHECK(pf::loadTuning(path, t) && std::fabs(t.pitch[69] - 69.5f) < 1e-4f);
        CHECK(pf::parseTun("[Tuning]\nnote 60=abc\nnote 69=6900\n", t) && t.pitch[60] == 60.0f);
        CHECK(pf::parseTun("[Tuning]\nnote=6000\nnotes 5=100\nnote 61=6150\n", t) && t.pitch[0] == 0.0f && t.pitch[5] == 5.0f);
        CHECK(!pf::parseScl("x\n1\nabc.def\n", t));
        CHECK(!pf::parseScl("x\n1\n3/2/7\n", t));
        CHECK(!pf::parseScl("x\n1\n101,5\n", t));
        const std::string tri = fixtureDir() + "/tritave.scl";
        std::ofstream(tri) << "tritave\n1\n3/1\n";
        CHECK(pf::loadTuning(tri, t) && std::fabs(t.pitch[61] - (60.0f + 12.0f * std::log2(3.0f))) < 1e-3f);
    }
    // Option lists (surface.py) follow the C++ enums: the label MPC shows is the mode the
    // engine runs. patch_map.cpp's static_asserts only compare counts; this pins the order.
    {
        struct Opt { int param, value; const char* text; };
        const Opt opts[] = {
            {pf::P_F1_TYPE, pf::F_OFF, "Off"}, {pf::P_F1_TYPE, pf::F_LP12, "LP12"}, {pf::P_F1_TYPE, pf::F_LP24, "LP24"},
            {pf::P_F1_TYPE, pf::F_BP, "BP"}, {pf::P_F1_TYPE, pf::F_HP12, "HP12"}, {pf::P_F1_TYPE, pf::F_HP24, "HP24"},
            {pf::P_F1_TYPE, pf::F_NOTCH, "Notch"}, {pf::P_F1_TYPE, pf::F_PEAK, "Peak"},
            {pf::P_F1_TYPE, pf::F_COMB_PLUS, "Comb+"}, {pf::P_F1_TYPE, pf::F_COMB_MINUS, "Comb-"},
            {pf::P_F1_TYPE, pf::F_VOWEL, "Vowel"},
            {pf::P_ENGINE, pf::EN_CLEAN, "Clean"}, {pf::P_ENGINE, pf::EN_NORMAL, "Normal"}, {pf::P_ENGINE, pf::EN_DIRTY, "Dirty"},
            {pf::P_O1_WAVE, pf::OW_TABLE, "Table"}, {pf::P_O1_WAVE, pf::OW_SINE, "Sine"},
            {pf::P_O1_WAVE, pf::OW_TRIANGLE, "Triangle"}, {pf::P_O1_WAVE, pf::OW_SAW, "Saw"},
            {pf::P_O1_WAVE, pf::OW_SQUARE, "Square"}, {pf::P_O1_WAVE, pf::OW_PULSE, "Pulse"}, {pf::P_O1_WAVE, pf::OW_NOISE, "Noise"},
            {pf::P_O1_PHMODE, pf::PH_RESET, "Reset"}, {pf::P_O1_PHMODE, pf::PH_RANDOM, "Random"}, {pf::P_O1_PHMODE, pf::PH_FREE, "Free"},
            {pf::P_O1_ROUTE, pf::RT_F1, "F1"}, {pf::P_O1_ROUTE, pf::RT_F2, "F2"}, {pf::P_O1_ROUTE, pf::RT_BOTH, "F1+F2"},
            {pf::P_O1_ROUTE, pf::RT_DIRECT, "Direct"},
            {pf::P_O1_SUB_WAVE, pf::CW_SINE, "Sine"}, {pf::P_O1_SUB_WAVE, pf::CW_TRIANGLE, "Triangle"},
            {pf::P_O1_SUB_WAVE, pf::CW_SAW, "Saw"}, {pf::P_O1_SUB_WAVE, pf::CW_SQUARE, "Square"},
            {pf::P_L1_WAVE, pf::LW_SINE, "Sine"}, {pf::P_L1_WAVE, pf::LW_TRIANGLE, "Triangle"},
            {pf::P_L1_WAVE, pf::LW_SAW_UP, "Saw Up"}, {pf::P_L1_WAVE, pf::LW_SAW_DOWN, "Saw Down"},
            {pf::P_L1_WAVE, pf::LW_SQUARE, "Square"}, {pf::P_L1_WAVE, pf::LW_SAMPLE_HOLD, "S&H"}, {pf::P_L1_WAVE, pf::LW_SMOOTH, "Smooth"},
            {pf::P_L1_TRIG, pf::LT_RETRIG, "Retrig"}, {pf::P_L1_TRIG, pf::LT_FREE, "Free"}, {pf::P_L1_TRIG, pf::LT_GLOBAL, "Global"},
            {pf::P_M1_MOD, pf::MM_NONE, "None"}, {pf::P_M1_MOD, pf::MM_CURVE, "Curve"}, {pf::P_M1_MOD, pf::MM_RECTIFY, "Rectify"},
            {pf::P_M1_MOD, pf::MM_QUANTIZE, "Quantize"}, {pf::P_M1_MOD, pf::MM_SAMPLE_HOLD, "S&H"}, {pf::P_M1_MOD, pf::MM_SLEW, "Slew"},
            {pf::P_ARP_DIR, pf::AD_UP, "Up"}, {pf::P_ARP_DIR, pf::AD_DOWN, "Down"}, {pf::P_ARP_DIR, pf::AD_UP_DOWN, "Up/Down"},
            {pf::P_ARP_DIR, pf::AD_DOWN_UP, "Down/Up"}, {pf::P_ARP_DIR, pf::AD_PLAYED, "Played"},
            {pf::P_ARP_DIR, pf::AD_RANDOM, "Random"}, {pf::P_ARP_DIR, pf::AD_CHORD, "Chord"},
            {pf::P_SEQ_MODE, pf::SQ_OFF, "Off"}, {pf::P_SEQ_MODE, pf::SQ_ARP, "Arp"}, {pf::P_SEQ_MODE, pf::SQ_SEQ, "Seq"},
            {pf::P_SH1_MODE, pf::SM_STEP, "Step"}, {pf::P_SH1_MODE, pf::SM_RAMP, "Ramp"}, {pf::P_SH1_MODE, pf::SM_SMOOTH, "Smooth"},
            {pf::P_VMODE, pf::VM_POLY, "Poly"}, {pf::P_VMODE, pf::VM_DUO, "Duo"}, {pf::P_VMODE, pf::VM_MONO, "Mono"},
            {pf::P_VMODE, pf::VM_LEGATO, "Legato"},
            {pf::P_STEAL, pf::ST_OLDEST, "Oldest"}, {pf::P_STEAL, pf::ST_QUIETEST, "Quietest"},
            {pf::P_STEAL, pf::ST_KEEP_LOW, "Keep low"}, {pf::P_STEAL, pf::ST_KEEP_HIGH, "Keep high"},
            {pf::P_GLIDE_MODE, pf::GL_OFF, "Off"}, {pf::P_GLIDE_MODE, pf::GL_ALWAYS, "Always"}, {pf::P_GLIDE_MODE, pf::GL_LEGATO, "Legato"},
        };
        const char* sources[pf::MS_COUNT] = {"None", "Env 1", "Env 2", "LFO 1", "LFO 2", "Velocity", "Note", "Mod Wheel",
                                             "Aftertouch", "Bend", "Random", "Alternate", "Gate", "Seq", "Shape 1", "Shape 2",
                                             "Shape 3", "Shape 4", "X1", "Y1", "X2", "Y2", "X3", "Y3", "X4", "Y4", "Breath",
                                             "Expression", "Constant"};
        const char* targets[pf::MT_COUNT] = {
            "Off", "Pitch", "Osc1 Pitch", "Osc2 Pitch", "Osc1 Pos", "Osc2 Pos", "Osc1 Level", "Osc2 Level", "Osc1 Pan",
            "Osc2 Pan", "Osc1 Detune", "Osc2 Detune", "Sub1 Level", "Sub2 Level", "Noise Level", "Noise Colour",
            "F1 Cutoff", "F2 Cutoff", "Cutoffs", "F1 Reso", "F2 Reso", "F1 Drive", "F2 Drive", "Env1 Attack",
            "Env1 Decay", "Env1 Sustain", "Env1 Release", "Env2 Attack", "Env2 Decay", "Env2 Sustain", "Env2 Release",
            "LFO1 Rate", "LFO2 Rate", "LFO1 Depth", "LFO2 Depth", "Volume", "Pan"};
        static_assert(pf::MS_BREATH == 26 && pf::MT_VOLUME == 35, "update the lists above with the enums");
        auto shows = [](int id, int value, const char* text) {
            return pf::paramDisplay(id, pf::paramNorm(id, static_cast<float>(value))) == text;
        };
        int wrong = 0;
        for (const Opt& o : opts) wrong += shows(o.param, o.value, o.text) ? 0 : 1;
        for (int i = 0; i < pf::MS_COUNT; ++i) wrong += shows(pf::P_M1_SRC, i, sources[i]) && shows(pf::P_M1_VIA, i, sources[i]) ? 0 : 1;
        for (int i = 0; i < pf::MT_COUNT; ++i) wrong += shows(pf::P_M1_T1, i, targets[i]) ? 0 : 1;
        CHECK(wrong == 0);
    }
    // Keys with control characters resolve to nothing ("..\0" passed the ".." check, and the
    // path then ended at the NUL: "<root>/..").
    {
        const std::vector<pf::Root> roots = {{"plugin", "/nonexistent"}};
        CHECK(pf::resolveKey(std::string("plugin:..\0", 10), roots).empty());
        CHECK(pf::resolveKey("plugin:a\nb.wav", roots).empty());
        CHECK(pf::resolveKey("plugin:a b.wav", roots) == "/nonexistent/a b.wav");
    }
}

} // namespace

void reviewTests() {
    engineFixes();
    noteGenFixes();
    surfaceFixes();
    fileFixes();
    noteGenFixes2();
    pluginFixes2();
}

} // namespace pft
