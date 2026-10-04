#pragma once
// A fake MPC: drives the plugin through its VST2 entry points the way MPC does (128-frame
// blocks, events with deltaFrames, 0..1 params), records what the plugin pushes back
// (audioMasterAutomate, audioMasterUpdateDisplay) and serves transport time.
#include "check.h"
#include "../plugin/vst2.h"
#include "../plugin/patch_map.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

extern "C" AEffect* VSTPluginMain(audioMasterCallback master);

namespace pft {

constexpr int kBlock = 128;
constexpr int kBlocksPerSec = 44100 / kBlock;

struct HostLog {
    int updates = 0;
    std::map<int, float> automated;   // index -> last value pushed
    std::map<int, int> automateCount;
    VstTimeInfo time{};
};

intptr_t hostMaster(AEffect* e, int32_t op, int32_t index, intptr_t, void*, float opt);

struct Host {
    AEffect* e;
    HostLog log;
    std::vector<float> L, R;   // last run, all samples
    bool finite = true;

    Host() : e(VSTPluginMain(hostMaster)) {
        e->user = &log;
        log.time.sampleRate = 44100.0;
        log.time.tempo = 120.0;
        log.time.timeSigNumerator = log.time.timeSigDenominator = 4;
        log.time.flags = vst::kVstTempoValid | vst::kVstPpqPosValid;
        e->dispatcher(e, vst::effOpen, 0, 0, nullptr, 0.0f);
    }
    ~Host() { e->dispatcher(e, vst::effClose, 0, 0, nullptr, 0.0f); }
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    void set(int id, float value) { e->setParameter(e, id, pf::paramNorm(id, value)); }
    void setN(int id, float n) { e->setParameter(e, id, n); }
    float get(int id) { return e->getParameter(e, id); }
    float value(int id) { return pf::paramValue(id, get(id)); }

    // A momentary button: press, then release (as MPC sends a tap).
    void press(int id) {
        e->setParameter(e, id, 1.0f);
        e->setParameter(e, id, 0.0f);
    }

    void midi(uint8_t st, uint8_t d1, uint8_t d2, int delta = 0) {
        VstMidiEvent ev{};
        ev.type = vst::kVstMidiType;
        ev.byteSize = sizeof ev;
        ev.deltaFrames = delta;
        ev.midiData[0] = st;
        ev.midiData[1] = d1;
        ev.midiData[2] = d2;
        VstEvents evs{};
        evs.numEvents = 1;
        evs.events[0] = reinterpret_cast<VstEvent*>(&ev);
        e->dispatcher(e, vst::effProcessEvents, 0, 0, &evs, 0.0f);
    }
    void on(int note, int vel = 100, int delta = 0) { midi(0x90, static_cast<uint8_t>(note), static_cast<uint8_t>(vel), delta); }
    void off(int note, int delta = 0) { midi(0x80, static_cast<uint8_t>(note), 0, delta); }

    // Renders `blocks` blocks (the transport advances if playing); returns the peak |sample|.
    float run(int blocks) {
        L.assign(static_cast<size_t>(blocks) * kBlock, 0.0f);
        R.assign(L.size(), 0.0f);
        float peak = 0.0f;
        for (int b = 0; b < blocks; ++b) {
            float* out[2] = {&L[static_cast<size_t>(b) * kBlock], &R[static_cast<size_t>(b) * kBlock]};
            e->processReplacing(e, nullptr, out, kBlock);
            if (log.time.flags & vst::kVstTransportPlaying) {
                log.time.samplePos += kBlock;
                log.time.ppqPos += kBlock / 44100.0 * log.time.tempo / 60.0;
            }
        }
        for (size_t i = 0; i < L.size(); ++i) {
            if (!std::isfinite(L[i]) || !std::isfinite(R[i])) finite = false;
            peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));
        }
        return peak;
    }

    std::string display(int id) {
        char b[256] = {};
        e->dispatcher(e, vst::effGetParamDisplay, id, 0, b, 0.0f);
        return b;
    }
    std::string name(int id) {
        char b[256] = {};
        e->dispatcher(e, vst::effGetParamName, id, 0, b, 0.0f);
        return b;
    }
    int voices() {   // from the status line ("VOICES n ..."), published every 0.5 s
        run(kBlocksPerSec / 2 + 2);
        return std::atoi(display(pf::P_STATUS).c_str() + 7);
    }

    std::string chunk() {
        void* data = nullptr;
        const intptr_t size = e->dispatcher(e, vst::effGetChunk, 0, 0, &data, 0.0f);
        return size > 0 && data ? std::string(static_cast<const char*>(data)) : std::string();
    }
    intptr_t load(const std::string& s) {
        return e->dispatcher(e, vst::effSetChunk, 0, static_cast<intptr_t>(s.size()), const_cast<char*>(s.data()), 0.0f);
    }

    // Renders blocks until cond() holds (the loader works on its own thread); false on timeout.
    template <class F> bool until(F cond, int ms = 3000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (!cond()) {
            if (std::chrono::steady_clock::now() > end) return false;
            run(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        run(8);   // let the pushes (automate / update display) go out
        return true;
    }

    // A plain, predictable voice: one sine oscillator, filters off.
    void bare() {
        set(pf::P_O1_POS, 0.0f);   // Classic frame 0 = sine
        set(pf::P_O1_UNI, 1);
        set(pf::P_O2_LEVEL, 0.0f);
        set(pf::P_F1_TYPE, pf::F_OFF);
        set(pf::P_F2_TYPE, pf::F_OFF);
    }
};

// Test fixtures: Serum-layout wavetable WAVs written by the tests themselves.
std::string fixtureDir();                    // per-run temp folder (removed at exit)
void writeTable(const std::string& path, int frames, int frameSize = 2048, bool float32 = true, bool marker = true);

void tablesTests();      // m1_test.cpp
void libraryTests();
void loaderTests();
void browserTests();
void steppingTests();
void voiceTests();        // m2_test.cpp
void oscillatorTests();   // m3_test.cpp
void filterTests();       // m4_test.cpp
void modulationTests();   // m5_test.cpp

} // namespace pft
