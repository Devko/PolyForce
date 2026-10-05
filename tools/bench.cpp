// CPU bench for the built plugin: dlopen()s the .so like MPC does, plays held chords at
// 44.1 kHz / 128-frame blocks and times every processReplacing call with the thread's own
// CPU clock. Same verdict rule as sd88me's tools/bench.sh (percent of the 2902 us block):
// PASS p99 <= 15% and max <= 50%, WARN p99 <= 35% and max <= 80%, else FAIL.
//
//   pfbench <plugin.so> [-v 1,2,4,8] [-u 1,2,4,8] [-s seconds] [-c cpu] [-t table.wav] [-m 1]
//
// Patch under test: both oscillators on, filter 1 LP24 with drive, filter 2 LP12, serial.
// -m 1 adds a busy modulation matrix (8 slots): LFO 1 on pitch and filter 1, LFO 2 on osc 1's
// position, filter 2 and the amp attack (envelope coefficients recomputed as it moves), env 2
// on osc 2's position, a slewed velocity and an S&H random slot.
// Hermetic: the plugin reads no user folders and saves nothing (favorites, recent lists).
// -t also times importing one wavetable WAV (the loader is linked in, not the .so's copy):
// what picking a table on the touchscreen would cost on this CPU; then plays the largest voice
// count with an LFO sweeping both positions, first on the built-in Classic table (fits the
// cache), then on that table (several MB): if the oscillators wait on memory, the second
// takes clearly longer.
// A profiling build of the plugin (make arm-bench-stages: polyforce_stages.so) also reports
// where each case's time goes, per render pass.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../dsp/mod.h"
#include "../dsp/wavetable.h"
#include "../plugin/vst2.h"
#include "param_ids.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <sched.h>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kBlock = 128;
constexpr double kBudgetUs = kBlock * 1e6 / 44100.0;   // 2902 us

VstTimeInfo g_time{};

intptr_t master(AEffect*, int32_t op, int32_t, intptr_t, void*, float) {
    if (op == 1) return 2400;   // audioMasterVersion
    if (op == vst::audioMasterGetTime) return reinterpret_cast<intptr_t>(&g_time);
    return 0;
}

double cpuUs() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) * 1e6 + static_cast<double>(ts.tv_nsec) * 1e-3;
}

double wallUs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) * 1e6 + static_cast<double>(ts.tv_nsec) * 1e-3;
}

double wallMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) * 1e3 + static_cast<double>(ts.tv_nsec) * 1e-6;
}

float norm(int id, float v) {
    const pf::ParamSpec& s = pf::PARAM_SPECS[id];
    const float n = s.curve == pf::Curve::Log ? std::log(v / s.lo) / std::log(s.hi / s.lo)
                                              : (s.hi > s.lo ? (v - s.lo) / (s.hi - s.lo) : 0.0f);
    return std::clamp(n, 0.0f, 1.0f);
}

std::vector<int> list(const char* s) {
    std::vector<int> out;
    for (const char* p = s; *p;) {
        out.push_back(std::atoi(p));
        while (*p && *p != ',') ++p;
        if (*p == ',') ++p;
    }
    return out;
}

std::string display(AEffect* e, int id) {
    char b[256] = {};
    e->dispatcher(e, vst::effGetParamDisplay, id, 0, b, 0.0f);
    return b;
}

void midi(AEffect* e, uint8_t st, uint8_t d1, uint8_t d2) {
    VstMidiEvent ev{};
    ev.type = vst::kVstMidiType;
    ev.byteSize = sizeof ev;
    ev.midiData[0] = st;
    ev.midiData[1] = d1;
    ev.midiData[2] = d2;
    VstEvents evs{};
    evs.numEvents = 1;
    evs.events[0] = reinterpret_cast<VstEvent*>(&ev);
    e->dispatcher(e, vst::effProcessEvents, 0, 0, &evs, 0.0f);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <plugin.so> [-v 1,2,4,8] [-u 1,2,4,8] [-s seconds] [-c cpu] [-t table.wav] [-m 1]\n", argv[0]);
        return 2;
    }
    std::vector<int> voices = {1, 2, 4, 8}, unison = {1, 2, 4, 8};
    double seconds = 3.0;
    int cpu = 1;
    const char* table = nullptr;
    bool busy = false;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "-m")) busy = std::atoi(argv[i + 1]) != 0;
        if (!std::strcmp(argv[i], "-v")) voices = list(argv[i + 1]);
        else if (!std::strcmp(argv[i], "-u")) unison = list(argv[i + 1]);
        else if (!std::strcmp(argv[i], "-s")) seconds = std::atof(argv[i + 1]);
        else if (!std::strcmp(argv[i], "-c")) cpu = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "-t")) table = argv[i + 1];
    }
    if (cpu >= 0) {   // like `taskset`: off MPC's isolated audio cores, one core for the whole run
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof set, &set) != 0) std::printf("note: could not pin to cpu %d\n", cpu);
    }
    // Empty roots and data dir: no scan of /media/AkaiForce, no favorites or recent written.
    setenv("PF_DATA_DIR", "", 1);
    setenv("PF_TABLE_ROOTS", "", 1);
    setenv("PF_PRESET_ROOTS", "", 1);
    setenv("PF_TUNING_ROOTS", "", 1);
    bool failed = false;    // something the run was asked to do didn't happen
    std::string tableKey;   // the plugin's key for it, once it imports
    if (table) {
        pf::Wavetable t;
        std::string err;
        const double a = wallMs();
        const bool ok = pf::loadWavetable(table, t, &err);
        const double ms = wallMs() - a;
        if (ok) {
            std::printf("wavetable import: %d frames in %.0f ms (%.2f ms/frame), %.1f MB\n", t.frames, ms,
                        ms / t.frames, static_cast<double>(t.data.size() * sizeof(float)) / (1024.0 * 1024.0));
            // The plugin's only table root becomes the table's folder (its "plugin" root).
            const std::string path = table;
            const size_t slash = path.rfind('/');
            const std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);
            if (dir.find(':') != std::string::npos) {   // the roots variable is a ':' list
                std::printf("table playback skipped: its folder name contains ':'\n");
                failed = true;
            } else {
                setenv("PF_TABLE_ROOTS", dir.c_str(), 1);
                tableKey = "plugin:" + (slash == std::string::npos ? path : path.substr(slash + 1));
            }
        } else {
            std::printf("wavetable import failed: %s\n", err.c_str());
            failed = true;
        }
    }

    void* so = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!so) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    using Main = AEffect* (*)(audioMasterCallback);
    auto entry = reinterpret_cast<Main>(dlsym(so, "VSTPluginMain"));
    using StageFn = int (*)(double*, const char**, int);
    const auto stages = reinterpret_cast<StageFn>(dlsym(so, "PolyForceStageTimes"));   // profiling build only
    if (!entry) {
        std::fprintf(stderr, "no VSTPluginMain\n");
        return 1;
    }

    g_time.sampleRate = 44100.0;
    g_time.tempo = 120.0;
    g_time.flags = vst::kVstTempoValid | vst::kVstPpqPosValid;

    const double t0 = wallMs();
    AEffect* e = entry(master);
    const double loadMs = wallMs() - t0;
    if (!e || e->magic != vst::kMagic) {
        std::fprintf(stderr, "VSTPluginMain failed\n");
        return 1;
    }
    e->dispatcher(e, vst::effOpen, 0, 0, nullptr, 0.0f);
    e->dispatcher(e, vst::effSetSampleRate, 0, 0, nullptr, 44100.0f);
    e->dispatcher(e, vst::effSetBlockSize, 0, kBlock, nullptr, 0.0f);
    e->dispatcher(e, vst::effMainsChanged, 0, 1, nullptr, 0.0f);

    auto set = [e](int id, float v) { e->setParameter(e, id, norm(id, v)); };
    set(pf::P_VOICES, static_cast<float>(pf::kParamMaxVoices));
    set(pf::P_O1_LEVEL, 0.8f);
    set(pf::P_O2_LEVEL, 0.6f);
    set(pf::P_F1_TYPE, 2);   // LP24
    set(pf::P_F1_DRIVE, 0.5f);
    set(pf::P_F2_TYPE, 1);   // LP12
    set(pf::P_F2_CUT, 6000.0f);
    set(pf::P_E2_POS, 0.3f);
    if (busy) {
        struct S { int src, tgt; float amt; int mod; };
        const S slots[8] = {{pf::MS_LFO1, pf::MT_PITCH, 0.05f, pf::MM_NONE},
                            {pf::MS_LFO2, pf::MT_O1_POS, 0.4f, pf::MM_NONE},
                            {pf::MS_LFO1, pf::MT_F1_CUT, 0.3f, pf::MM_NONE},
                            {pf::MS_LFO2, pf::MT_F2_CUT, -0.3f, pf::MM_NONE},
                            {pf::MS_ENV2, pf::MT_O2_POS, 0.5f, pf::MM_NONE},
                            {pf::MS_VELOCITY, pf::MT_CUT, 0.2f, pf::MM_SLEW},
                            {pf::MS_RANDOM, pf::MT_O1_PAN, 0.5f, pf::MM_SAMPLE_HOLD},
                            {pf::MS_LFO2, pf::MT_E1_A, 0.5f, pf::MM_NONE}};
        const int stride = pf::P_M2_SRC - pf::P_M1_SRC;
        for (int k = 0; k < 8; ++k) {
            set(pf::P_M1_SRC + k * stride, static_cast<float>(slots[k].src));
            set(pf::P_M1_T1 + k * stride, static_cast<float>(slots[k].tgt));
            set(pf::P_M1_A1 + k * stride, slots[k].amt);
            set(pf::P_M1_MOD + k * stride, static_cast<float>(slots[k].mod));
        }
        set(pf::P_L1_RATE, 5.0f);
        set(pf::P_L2_RATE, 0.3f);
        std::printf("busy matrix: 8 slots, both LFOs\n");
    }

    std::vector<float> L(kBlock), R(kBlock);
    float* out[2] = {L.data(), R.data()};
    const int warm = 44100 / kBlock / 2;
    const int blocks = std::max(1, static_cast<int>(seconds * 44100.0 / kBlock));

    std::printf("plugin: %s   load (VSTPluginMain, tables): %.0f ms   %d blocks/case\n", argv[1], loadMs, blocks);
    if (stages) std::printf("stage timing build: per-pass wall time on each case's second line (us per block)\n");
    const char* header = "voices unison  p50_us  p99_us  max_us   p99%   max%  verdict\n";
    int worst = 0;
    auto runCase = [&](int v, int u) {
        set(pf::P_O1_UNI, static_cast<float>(u));
        set(pf::P_O2_UNI, static_cast<float>(u));
        for (int n = 0; n < v; ++n) midi(e, 0x90, static_cast<uint8_t>(36 + n * 3), 100);
        for (int b = 0; b < warm; ++b) e->processReplacing(e, nullptr, out, kBlock);
        double st[8];
        const char* names[8];
        if (stages) stages(st, names, 8);   // drops the warm-up's
        std::vector<double> t(static_cast<size_t>(blocks));
        double wall = 0.0;   // the stages are wall time: so is their "rest"
        for (int b = 0; b < blocks; ++b) {
            const double w = wallUs(), a = cpuUs();
            e->processReplacing(e, nullptr, out, kBlock);
            t[static_cast<size_t>(b)] = cpuUs() - a;
            wall += wallUs() - w;
        }
        const int ns = stages ? stages(st, names, 8) : 0;
        midi(e, 0xB0, 120, 0);   // all sound off before the next case
        e->processReplacing(e, nullptr, out, kBlock);

        std::sort(t.begin(), t.end());
        const double p50 = t[t.size() / 2], p99 = t[t.size() * 99 / 100], mx = t.back();
        const double p99p = 100.0 * p99 / kBudgetUs, maxp = 100.0 * mx / kBudgetUs;
        const int verdict = (p99p <= 15.0 && maxp <= 50.0) ? 0 : (p99p <= 35.0 && maxp <= 80.0) ? 1 : 2;
        worst = std::max(worst, verdict);
        std::printf("%6d %6d %7.0f %7.0f %7.0f %6.1f %6.1f  %s\n", v, u, p50, p99, mx, p99p, maxp,
                    verdict == 0 ? "PASS" : verdict == 1 ? "WARN" : "FAIL");
        if (ns > 0) {   // the rest: the plugin glue, note generator, events, the master volume
            double staged = 0.0;
            std::printf("              ");
            for (int i = 0; i < ns; ++i) {
                std::printf(" %s %.1f", names[i], st[i] / blocks);
                staged += st[i] / blocks;
            }
            std::printf("  rest %.1f\n", wall / blocks - staged);
        }
        std::fflush(stdout);
    };
    std::printf("%s", header);
    for (int u : unison)
        for (int v : voices) runCase(v, u);

    if (!tableKey.empty()) {
        // Both positions swept by LFO 1 (slot 9, clear of the busy matrix's): the frames read
        // keep changing, as when a table is played rather than parked on one frame.
        const int stride = pf::P_M2_SRC - pf::P_M1_SRC, slot = 8 * stride;
        set(pf::P_M1_SRC + slot, static_cast<float>(pf::MS_LFO1));
        set(pf::P_M1_T1 + slot, static_cast<float>(pf::MT_O1_POS));
        set(pf::P_M1_A1 + slot, 0.5f);
        set(pf::P_M1_T2 + slot, static_cast<float>(pf::MT_O2_POS));
        set(pf::P_M1_A2 + slot, 0.5f);
        const int v = *std::max_element(voices.begin(), voices.end());
        std::printf("\nplayback, positions swept, table %s\n", display(e, pf::P_O1_TABLE).c_str());
        std::printf("%s", header);
        for (int u : unison) runCase(v, u);

        void* data = nullptr;   // a project chunk changes only what it names: the two tables
        const intptr_t size = e->dispatcher(e, vst::effGetChunk, 0, 0, &data, 0.0f);
        const std::string state = size > 0 && data ? std::string(static_cast<const char*>(data), static_cast<size_t>(size)) : "";
        const std::string tables = state.substr(0, state.find('\n')) + "\no1_table=" + tableKey + "\no2_table=" + tableKey + "\n";
        const std::string before = display(e, pf::P_O1_TABLE);
        e->dispatcher(e, vst::effSetChunk, 0, static_cast<intptr_t>(tables.size()), const_cast<char*>(tables.data()), 0.0f);
        // The loader imports it on its own thread: 0 = not yet ("LOADING ..."), 1 = loaded,
        // -1 = failed (the stepper says MISSING; the plugin plays its fallback).
        auto tableState = [&](int id) {
            const std::string d = display(e, id);
            if (d == before || d.find("LOADING") != std::string::npos) return 0;
            return d.find("MISSING") != std::string::npos ? -1 : 1;
        };
        const double until = wallMs() + 20000.0;
        while ((tableState(pf::P_O1_TABLE) == 0 || tableState(pf::P_O2_TABLE) == 0) && wallMs() < until) {
            e->processReplacing(e, nullptr, out, kBlock);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (tableState(pf::P_O1_TABLE) != 1 || tableState(pf::P_O2_TABLE) != 1) {
            std::printf("\nthe plugin did not load %s (%s)\n", tableKey.c_str(), display(e, pf::P_O1_TABLE).c_str());
            failed = true;
        } else {
            std::printf("\nplayback, positions swept, table %s\n", display(e, pf::P_O1_TABLE).c_str());
            std::printf("%s", header);
            for (int u : unison) runCase(v, u);
        }
    }
    std::printf("worst case: %s\n", worst == 0 ? "PASS" : worst == 1 ? "WARN" : "FAIL");
    e->dispatcher(e, vst::effClose, 0, 0, nullptr, 0.0f);
    dlclose(so);
    return failed ? 1 : 0;   // the run itself went wrong (a verdict is not a failure)
}
