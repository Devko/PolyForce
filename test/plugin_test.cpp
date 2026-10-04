// Offline checks: the wavetables, then the whole plugin driven through its VST2 entry
// points the way MPC drives it (128-frame blocks, events with deltaFrames, 0..1 params).
// Built with ASan/UBSan by `make test`.
#include "host.h"
#include "../dsp/wavetable.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace pft {
int g_fail = 0, g_pass = 0;

intptr_t hostMaster(AEffect* e, int32_t op, int32_t index, intptr_t, void*, float opt) {
    HostLog* log = e ? static_cast<HostLog*>(e->user) : nullptr;
    if (op == 1) return 2400;   // audioMasterVersion
    if (!log) return 0;
    if (op == vst::audioMasterUpdateDisplay) ++log->updates;
    if (op == vst::audioMasterAutomate) {
        log->automated[index] = opt;
        ++log->automateCount[index];
    }
    if (op == vst::audioMasterGetTime) return reinterpret_cast<intptr_t>(&log->time);
    return 0;
}

std::string fixtureDir() {
    static const std::string dir = [] {
        char tmpl[] = "/tmp/pftest.XXXXXX";
        const char* d = mkdtemp(tmpl);
        return std::string(d ? d : "/tmp/pftest");
    }();
    return dir;
}
} // namespace pft

namespace {
using namespace pft;

void testTables() {
    const auto& t = pf::builtinTables();
    CHECK(t.size() == 4);
    for (const auto& w : t) {
        CHECK(w.frames == 16);
        for (int f = 0; f < w.frames; ++f) {
            const float* m0 = w.get(f, 0);
            float peak = 0.0f;
            for (int i = 0; i < pf::kTableSize; ++i) peak = std::max(peak, std::fabs(m0[i]));
            CHECK(peak > 0.99f && peak < 1.01f);
            bool guards = true;   // every level ends in a guard sample = its sample 0
            for (int k = 0; k < pf::kMipLevels; ++k) {
                const float* lv = w.get(f, k);
                guards = guards && lv[pf::mipLength(k)] == lv[0];
            }
            CHECK(guards);
            // the top level keeps only the fundamental: at most one rise through zero (none
            // when the frame has no fundamental, e.g. Sync at an exact 2x or 8x ratio)
            // (a silent level holds ~1e-16 FFT crosstalk from its paired frame: skip it)
            const float* top = w.get(f, pf::kMipLevels - 1);
            const int topLen = pf::mipLength(pf::kMipLevels - 1);
            float topPeak = 0.0f;
            for (int i = 0; i < topLen; ++i) topPeak = std::max(topPeak, std::fabs(top[i]));
            int rises = 0;
            for (int i = 0; i < topLen; ++i) rises += (top[i] < 0.0f && top[i + 1] >= 0.0f) ? 1 : 0;
            CHECK(rises <= 1 || topPeak < 1e-6f);
        }
    }
    CHECK(pf::mipFor(1e-4f) == 0);
    CHECK(pf::mipFor(0.45f) == pf::kMipLevels - 1);
    CHECK(pf::mipFor(0.01f) == 5);   // 0.59 / 0.01 = 59 allowed -> the 32-harmonic level
    int last = 0;
    bool monotonic = true;
    for (float inc = 1e-5f; inc < 0.5f; inc *= 1.1f) {
        const int k = pf::mipFor(inc);
        monotonic = monotonic && k >= last && (pf::kMaxHarmonic >> k) * inc <= pf::kAliasLimit + 1e-6f;
        last = k;
    }
    CHECK(monotonic);   // ...and the chosen level never exceeds the alias limit (until the last one)
}

void testDisplay() {
    Host h;
    CHECK(h.display(pf::P_F1_CUT) == "1.20 kHz");
    CHECK(h.display(pf::P_E1_A) == "3.0 ms");
    CHECK(h.display(pf::P_E1_D) == "400 ms");
    CHECK(h.display(pf::P_VOLUME) == "-6.0 dB");
    CHECK(h.display(pf::P_O1_TABLE) == "Built-in / Classic");
    CHECK(h.display(pf::P_F1_TYPE) == "LP24");
    CHECK(h.display(pf::P_O2_FINE) == "+7 ct");
    CHECK(h.display(pf::P_VOICES) == std::to_string(pf::kMaxVoices));
    h.set(pf::P_O1_UNI, 99);   // knob ranges stop at the engine's ceilings
    CHECK(h.display(pf::P_O1_UNI) == std::to_string(pf::kMaxUnison));
    h.set(pf::P_VOLUME, -60.0f);
    CHECK(h.display(pf::P_VOLUME) == "-inf dB");
    // the status line ignores host sets (MPC sets param 0 right after loading a plugin)
    h.e->setParameter(h.e, pf::P_STATUS, 1.0f);
    CHECK(h.get(pf::P_STATUS) == 0.0f);
}

void testTuning() {
    Host h;
    h.bare();
    CHECK(h.run(4) == 0.0f);   // nothing playing: digital silence
    h.on(69);
    h.run(kBlocksPerSec);      // 1 s of A4
    int rises = 0;
    for (size_t i = 1; i < h.L.size(); ++i) rises += (h.L[i - 1] < 0.0f && h.L[i] >= 0.0f) ? 1 : 0;
    std::printf("  A4 -> %d cycles in %zu samples (expect ~%.0f)\n", rises, h.L.size(), 440.0 * h.L.size() / 44100.0);
    CHECK(std::abs(rises - static_cast<int>(440.0 * h.L.size() / 44100.0)) <= 2);
    CHECK(h.finite);
}

void testNoteLifecycle() {
    Host h;
    h.on(60);
    const float peak = h.run(kBlocksPerSec / 2);
    CHECK(peak > 0.01f && peak < 2.0f);
    CHECK(h.voices() == 1);
    h.off(60);
    h.run(kBlocksPerSec * 2);   // release is 300 ms
    CHECK(h.voices() == 0);
    CHECK(h.run(4) == 0.0f);
    CHECK(h.finite);
}

void testSampleAccurate() {
    Host h;
    h.bare();
    h.on(69, 127, 100);
    h.run(1);
    float before = 0.0f, after = 0.0f;
    for (int i = 0; i < 100; ++i) before = std::max(before, std::fabs(h.L[static_cast<size_t>(i)]));
    for (int i = 100; i < kBlock; ++i) after = std::max(after, std::fabs(h.L[static_cast<size_t>(i)]));
    CHECK(before == 0.0f);
    CHECK(after > 1e-6f);
}

void testPolyphony() {
    Host h;
    for (int n = 0; n < 20; ++n) h.on(40 + n);
    CHECK(h.voices() == pf::kMaxVoices);   // 20 notes, the rest stolen
    h.midi(0xB0, 123, 0);      // all notes off
    h.run(kBlocksPerSec * 2);
    CHECK(h.voices() == 0);

    h.set(pf::P_VOICES, 4);
    for (int n = 0; n < 6; ++n) h.on(60 + n);
    CHECK(h.voices() == 4);
    h.on(60);                  // same note again reuses its voice
    CHECK(h.voices() == 4);
    CHECK(h.finite);
}

void testSustainPedal() {
    Host h;
    h.midi(0xB0, 64, 127);
    h.on(60);
    h.run(8);
    h.off(60);
    CHECK(h.voices() == 1);    // held by the pedal
    h.midi(0xB0, 64, 0);
    h.run(kBlocksPerSec * 2);
    CHECK(h.voices() == 0);
}

// Everything at its extremes: must stay finite and bounded.
void testStress() {
    for (int type = 0; type < pf::kNumFilterTypes; ++type) {
        Host h;
        for (int o = 0; o < 2; ++o) {
            const int d = o * (pf::P_O2_TABLE - pf::P_O1_TABLE);
            h.set(pf::P_O1_UNI + d, pf::kMaxUnison);
            h.set(pf::P_O1_DETUNE + d, 1.0f);
            h.set(pf::P_O1_WIDTH + d, 1.0f);
            h.set(pf::P_O1_LEVEL + d, 1.0f);
            h.set(pf::P_O1_OCT + d, o ? 3.0f : -3.0f);
        }
        h.set(pf::P_ROUTING, static_cast<float>(type & 1));
        for (int f = 0; f < 2; ++f) {
            const int d = f * (pf::P_F2_TYPE - pf::P_F1_TYPE);
            h.set(pf::P_F1_TYPE + d, static_cast<float>(type));
            h.set(pf::P_F1_RES + d, 1.0f);
            h.set(pf::P_F1_DRIVE + d, 1.0f);
            h.set(pf::P_F1_ENV + d, f ? -1.0f : 1.0f);
            h.set(pf::P_F1_CUT + d, f ? 20000.0f : 20.0f);
        }
        h.set(pf::P_E2_POS, 1.0f);
        h.set(pf::P_VOLUME, 6.0f);
        for (int n : {0, 24, 60, 96, 127}) h.on(n, 127);
        h.midi(0xE0, 0x7F, 0x7F);   // full bend up
        const float peak = h.run(kBlocksPerSec / 2);
        h.midi(0xE0, 0, 0);
        const float peak2 = h.run(kBlocksPerSec / 2);
        CHECK(h.finite);   // loud is fine here (Q ~18 on a clipped input), runaway is not
        CHECK(peak < 200.0f && peak2 < 200.0f);
    }
}

void testChunk() {
    Host a;
    a.set(pf::P_F1_CUT, 333.0f);
    a.set(pf::P_O2_UNI, 7);
    a.set(pf::P_ROUTING, 1);
    void* data = nullptr;
    const intptr_t size = a.e->dispatcher(a.e, vst::effGetChunk, 0, 0, &data, 0.0f);
    CHECK(size > 0 && data != nullptr);
    Host b;
    CHECK(b.e->dispatcher(b.e, vst::effSetChunk, 0, size, data, 0.0f) == 1);
    bool same = true;
    for (int i = 1; i < pf::P_COUNT; ++i) same = same && std::fabs(a.get(i) - b.get(i)) < 1e-5f;
    CHECK(same);
    CHECK(b.display(pf::P_F1_CUT) == "333 Hz");
    const std::string text(static_cast<const char*>(data));   // real values, not knob positions
    CHECK(text.find("o2_uni=7\n") != std::string::npos);
    CHECK(text.find("f1_cut=333\n") != std::string::npos);
    const char junk[] = "not a polyforce state";
    CHECK(b.e->dispatcher(b.e, vst::effSetChunk, 0, sizeof junk, const_cast<char*>(junk), 0.0f) == 0);

    auto load = [&b](const std::string& s) {
        return b.e->dispatcher(b.e, vst::effSetChunk, 0, static_cast<intptr_t>(s.size()), const_cast<char*>(s.data()), 0.0f);
    };
    // a project saved when unison went to 16 comes back at the new ceiling, not remapped
    CHECK(load("polyforce 2\no1_uni=16\nf2_cut=-5\nvolume=nan\n") == 1);
    CHECK(b.display(pf::P_O1_UNI) == std::to_string(pf::kMaxUnison));
    CHECK(b.display(pf::P_F2_CUT) == "20 Hz");
    CHECK(b.display(pf::P_VOLUME) == "-6.0 dB");   // garbage ignored, the value kept
    // version 1 (0..1 values) still loads
    CHECK(load("polyforce 1\no2_uni=0\nf1_res=1\n") == 1);
    CHECK(b.display(pf::P_O2_UNI) == "1");
    CHECK(b.display(pf::P_F1_RES) == "100%");
}

// One imported table (first .wav under $PF_WAVETABLES) through the loader and the engine,
// under the sanitizers. The full-folder sweep is `make test-tables`.
void testImportedTable() {
    namespace fs = std::filesystem;
    const char* dir = std::getenv("PF_WAVETABLES");
    std::error_code ec;
    if (!dir || !fs::is_directory(dir, ec)) {
        std::printf("  skipped: set PF_WAVETABLES to a folder of wavetable WAVs\n");
        return;
    }
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec))
        if (e.is_regular_file() && e.path().extension() == ".wav") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    CHECK(!files.empty());
    if (files.empty()) return;

    pf::Wavetable t;
    std::string err;
    CHECK(pf::loadWavetable(files.front().string(), t, &err));
    std::printf("  %s: %d frames %s\n", files.front().filename().string().c_str(), t.frames, err.c_str());
    CHECK(t.frames >= 1 && t.frames <= pf::kMaxFrames);

    pf::Synth s;
    pf::Patch p;
    p.osc[0].table = &t;
    p.osc[0].unison = 8;
    p.env2Pos = 1.0f;
    s.setPatch(p);
    s.noteOn(48, 100);
    s.noteOn(96, 100);
    float L[kBlock], R[kBlock], peak = 0.0f;
    bool finite = true;
    for (int b = 0; b < kBlocksPerSec; ++b) {
        s.render(L, R, kBlock);
        for (int i = 0; i < kBlock; ++i) {
            finite = finite && std::isfinite(L[i]) && std::isfinite(R[i]);
            peak = std::max(peak, std::fabs(L[i]));
        }
    }
    CHECK(finite);
    CHECK(peak > 1e-3f);

    // the loader refuses what it can't use instead of guessing
    pf::Wavetable junk;
    CHECK(!pf::loadWavetable("/nonexistent.wav", junk, &err));
    CHECK(!pf::loadWavetable(__FILE__, junk, &err) && err == "not a WAV file");
}

} // namespace

int main() {
    // Every table root, data file and preset lives in a throwaway folder: the tests never touch
    // the build tree or the user's files.
    const std::string fx = fixtureDir();
    setenv("PF_DATA_DIR", (fx + "/data").c_str(), 1);
    setenv("PF_TABLE_ROOTS", (fx + "/plugin:" + fx + "/ssd").c_str(), 1);
    setenv("PF_PRESET_ROOTS", (fx + "/presets").c_str(), 1);
    setenv("PF_TUNING_ROOTS", (fx + "/tunings").c_str(), 1);
    std::filesystem::create_directories(fx + "/data");

    std::printf("== tables\n");
    testTables();
    tablesTests();
    std::printf("== library\n");
    libraryTests();
    std::printf("== display\n");
    testDisplay();
    std::printf("== stepping\n");
    steppingTests();
    std::printf("== tuning\n");
    testTuning();
    std::printf("== note lifecycle\n");
    testNoteLifecycle();
    std::printf("== sample-accurate events\n");
    testSampleAccurate();
    std::printf("== polyphony + stealing\n");
    testPolyphony();
    std::printf("== sustain pedal\n");
    testSustainPedal();
    std::printf("== stress\n");
    testStress();
    std::printf("== chunk\n");
    testChunk();
    std::printf("== loader\n");
    loaderTests();
    std::printf("== browser\n");
    browserTests();
    std::printf("== imported wavetable\n");
    testImportedTable();
    std::error_code ec;
    std::filesystem::remove_all(fx, ec);
    std::printf("%s: %d passed, %d failed\n", g_fail ? "FAILED" : "PASSED", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
