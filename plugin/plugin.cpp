// PolyForce as a VST2 instrument for MPC OS (Force / MPC standalone).
//
// The engine (dsp/synth.h) behind the touchscreen pages generated from surface/surface.py;
// plugin/surface.* decides what every parameter does, plugin/loader.* loads wavetables in the
// background, and the status line carries a CPU meter so the cost can be read on the device.
//
// Threads (RackForcePlugin's MPC_PLUGIN_SPEC.md §2.7): processReplacing runs on one of MPC's
// audio workers (which one changes between calls, instances run concurrently); parameters,
// display text and chunks come from MPC's UI side; the loader has its own worker. Host
// callbacks are only made from processReplacing.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "vst2.h"
#include "param_ids.h"
#include "patch_map.h"
#include "loader.h"
#include "library.h"
#include "surface.h"
#include "../dsp/synth.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#if defined(__SSE__) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace {

using namespace pf;

constexpr size_t kTextCap = 48;   // JUCE reads names/display text into 256 bytes; stay well inside
constexpr int kMaxMidi = 512;
constexpr float kBendRange = 2.0f;   // semitones
constexpr float kSampleRate = 44100.0f;   // MPC OS always runs 44.1 kHz (spec §2.7)
constexpr int kScratch = 512;
constexpr const char* kStateMagic = "polyforce ";
constexpr int kStateVersion = 3;

// Denormals (the tails of decaying filters and envelopes) are slow on the VFP unit. Flush
// them to zero for our block only and hand MPC's worker back its own FP mode.
class FlushDenormals {
public:
    FlushDenormals() {
#if defined(__arm__)
        asm volatile("vmrs %0, fpscr" : "=r"(saved_));
        asm volatile("vmsr fpscr, %0" : : "r"(saved_ | (1u << 24)));   // FZ
#elif defined(__SSE__) || defined(__x86_64__)
        saved_ = _mm_getcsr();
        _mm_setcsr(saved_ | 0x8040);   // FTZ | DAZ
#endif
    }
    ~FlushDenormals() {
#if defined(__arm__)
        asm volatile("vmsr fpscr, %0" : : "r"(saved_));
#elif defined(__SSE__) || defined(__x86_64__)
        _mm_setcsr(saved_);
#endif
    }
    FlushDenormals(const FlushDenormals&) = delete;
    FlushDenormals& operator=(const FlushDenormals&) = delete;

private:
    uint32_t saved_ = 0;
};

double threadCpuUs() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) * 1e6 + static_cast<double>(ts.tv_nsec) * 1e-3;
}

struct RawMidi {
    int32_t delta;
    uint8_t status, d1, d2;
};

struct Plugin {
    AEffect             fx;          // must stay the first member: MPC hands us &fx back
    audioMasterCallback master = nullptr;
    Loader              loader{{tableSlotType(), tableSlotType()}};
    Surface             surface{loader};
    std::atomic<bool>   panic{false};
    pf::Synth           synth{kSampleRate};
    std::string         chunk;       // effGetChunk buffer: must outlive the call

    // audio thread only
    float   snapshot[P_COUNT] = {};
    RawMidi midi[kMaxMidi] = {};
    int     nMidi = 0;
    float   scratch[2][kScratch] = {};

    // CPU meter: the audio thread sums its own CPU time against the real-time budget and
    // publishes twice a second; the status line is formatted on the UI thread.
    double           winUs = 0.0, winBudgetUs = 0.0, winPeak = 0.0;
    std::atomic<int> shownVoices{0}, shownAvg{0}, shownPeak{0};   // percent
    int              lastVoices = -1, lastAvg = -1, lastPeak = -1;

    Plugin() {
        // Loaded tables show in the stepper texts and the browser; a fresh one goes on the
        // Recent list. Runs on the loader's worker.
        loader.setListener([this](int, const std::string& key, bool ok) {
            if (ok && key.compare(0, 8, "builtin:") != 0) tableLibrary().touchRecent(key);
            surface.refresh();
        });
    }
    ~Plugin() { loader.stop(); }   // its listener uses the surface, destroyed before it
};

Plugin* self(AEffect* e) { return static_cast<Plugin*>(e->object); }

void copyStr(void* dst, const std::string& s, size_t cap) {
    if (!dst || cap == 0) return;
    std::strncpy(static_cast<char*>(dst), s.c_str(), cap - 1);
    static_cast<char*>(dst)[cap - 1] = 0;
}

std::string statusText(const Plugin* p) {
    const std::string busy = p->surface.busyText();
    if (!busy.empty()) return busy;
    char b[64];
    std::snprintf(b, sizeof b, "VOICES %d   CPU %d%%   PEAK %d%%", p->shownVoices.load(),
                  p->shownAvg.load(), p->shownPeak.load());
    return b;
}

// --- parameters (UI thread) ---------------------------------------------------------------

float getParameter(AEffect* e, int32_t i) { return self(e)->surface.get(i); }

void setParameter(AEffect* e, int32_t i, float v) { self(e)->surface.set(i, v); }

// --- state --------------------------------------------------------------------------------
// "polyforce 3": key=value lines of REAL values (Hz, seconds, voice counts, option index) for
// every sound parameter, plus the tables by key. Survives parameters being added or reordered
// AND ranges changing (a 0..1 value would silently move: unison 4 of 1..16 reads back as 2 of
// 1..8). Version 1 stored 0..1 values; version 2 chose a built-in table by index (o1_wave).

std::string saveState(const Plugin* p) {
    std::string s = std::string(kStateMagic) + std::to_string(kStateVersion) + "\n";
    char b[96];
    for (int i = 0; i < P_COUNT; ++i) {
        if (PARAM_INFO[i].kind != Kind::Synth) continue;
        std::snprintf(b, sizeof b, "%s=%.6g\n", PARAM_INFO[i].key,   // 6 digits: 333 Hz, not 332.9999
                      static_cast<double>(paramValue(i, p->surface.get(i))));
        s += b;
    }
    for (int o = 0; o < 2; ++o) s += "o" + std::to_string(o + 1) + "_table=" + p->surface.tableKey(o) + "\n";
    return s;
}

bool loadState(Plugin* p, const std::string& s) {
    if (s.compare(0, std::strlen(kStateMagic), kStateMagic) != 0) return false;
    const int version = std::atoi(s.c_str() + std::strlen(kStateMagic));
    const bool normalised = version < 2;
    std::string tables[2] = {"builtin:Classic", "builtin:Classic"};
    size_t at = s.find('\n');
    while (at != std::string::npos && at + 1 < s.size()) {
        const size_t end = s.find('\n', at + 1);
        const std::string line = s.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
        at = end;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        if (key == "o1_table" || key == "o2_table") {
            tables[key[1] - '1'] = val;
            continue;
        }
        if (version < 3 && (key == "o1_wave" || key == "o2_wave")) {   // v2: a built-in by index
            const auto& b = builtinTables();
            const float v = std::strtof(val.c_str(), nullptr);
            const int idx = std::isfinite(v) ? std::clamp(static_cast<int>(normalised ? std::lround(v * 3.0f) : std::lround(v)), 0,
                                                          static_cast<int>(b.size()) - 1) : 0;
            tables[key[1] - '1'] = "builtin:" + b[static_cast<size_t>(idx)].name;
            continue;
        }
        for (int i = 0; i < P_COUNT; ++i)
            if (PARAM_INFO[i].kind == Kind::Synth && key == PARAM_INFO[i].key) {
                const float v = std::strtof(val.c_str(), nullptr);
                if (std::isfinite(v)) p->surface.setValue(i, normalised ? std::clamp(v, 0.0f, 1.0f) : paramNorm(i, v));
                break;
            }
    }
    for (int o = 0; o < 2; ++o) p->surface.setTable(o, tables[o], true);
    p->surface.refresh();
    return true;
}

// --- audio thread -------------------------------------------------------------------------

void handleMidi(pf::Synth& s, const RawMidi& m) {
    switch (m.status & 0xF0) {
        case 0x90:
            if (m.d2) s.noteOn(m.d1, m.d2);
            else s.noteOff(m.d1);
            break;
        case 0x80: s.noteOff(m.d1); break;
        case 0xB0:
            if (m.d1 == 64) s.sustain(m.d2 >= 64);
            else if (m.d1 == 120) s.reset();
            else if (m.d1 == 123) s.allNotesOff();
            break;
        case 0xE0:
            s.pitchBend(static_cast<float>((m.d2 << 7 | m.d1) - 8192) / 8192.0f * kBendRange);
            break;
        default: break;
    }
}

void runBlock(Plugin* p, float* L, float* R, int n) {
    p->surface.snapshot(p->snapshot);
    Patch patch = patchFromParams(p->snapshot);
    for (int o = 0; o < 2; ++o)   // read once per block: valid until blockDone() below
        patch.osc[o].table = static_cast<const Wavetable*>(p->loader.live(o));
    p->synth.setPatch(patch);
    if (p->panic.exchange(false)) p->synth.reset();

    // Events in time order (insertion sort: no allocation; MPC already sends them sorted),
    // each one applied at its own sample.
    for (int i = 1; i < p->nMidi; ++i)
        for (int j = i; j > 0 && p->midi[j].delta < p->midi[j - 1].delta; --j) std::swap(p->midi[j], p->midi[j - 1]);
    int pos = 0;
    for (int i = 0; i < p->nMidi; ++i) {
        const int at = std::clamp(static_cast<int>(p->midi[i].delta), 0, n);
        if (at > pos) {
            p->synth.render(L + pos, R + pos, at - pos);
            pos = at;
        }
        handleMidi(p->synth, p->midi[i]);
    }
    if (pos < n) p->synth.render(L + pos, R + pos, n - pos);
    p->nMidi = 0;
}

void meter(Plugin* p, double us, int n) {
    const double budget = static_cast<double>(n) * 1e6 / kSampleRate;
    p->winUs += us;
    p->winBudgetUs += budget;
    p->winPeak = std::max(p->winPeak, us / budget);
    if (p->winBudgetUs < 500000.0) return;

    const int avg = static_cast<int>(std::lround(100.0 * p->winUs / p->winBudgetUs));
    const int peak = static_cast<int>(std::lround(100.0 * p->winPeak));
    const int voices = p->synth.activeVoices();
    p->winUs = p->winBudgetUs = p->winPeak = 0.0;
    p->shownAvg.store(avg);
    p->shownPeak.store(peak);
    p->shownVoices.store(voices);
    if (avg != p->lastAvg || peak != p->lastPeak || voices != p->lastVoices) {
        p->lastAvg = avg;
        p->lastPeak = peak;
        p->lastVoices = voices;
        if (p->master) p->master(&p->fx, vst::audioMasterUpdateDisplay, 0, 0, nullptr, 0.0f);
    }
}

void hostAutomate(void* ctx, int index, float value) {
    Plugin* p = static_cast<Plugin*>(ctx);
    if (p->master) p->master(&p->fx, vst::audioMasterAutomate, index, 0, nullptr, value);
}

void hostUpdate(void* ctx) {
    Plugin* p = static_cast<Plugin*>(ctx);
    if (p->master) p->master(&p->fx, vst::audioMasterUpdateDisplay, 0, 0, nullptr, 0.0f);
}

void processReplacing(AEffect* e, float** /*in*/, float** out, int32_t n) {
    if (!out || !out[0] || !out[1] || n <= 0) return;
    Plugin* p = self(e);
    FlushDenormals ftz;
    const double t0 = threadCpuUs();
    try {
        runBlock(p, out[0], out[1], n);
    } catch (...) {   // nothing may throw into MPC: an escaping exception ends the whole process
        std::memset(out[0], 0, sizeof(float) * static_cast<size_t>(n));
        std::memset(out[1], 0, sizeof(float) * static_cast<size_t>(n));
    }
    p->loader.blockDone();   // the tables read at block start are no longer in use
    p->surface.notify(hostAutomate, hostUpdate, p);
    meter(p, threadCpuUs() - t0, n);
}

// Legacy accumulating entry point (MPC uses processReplacing).
void process(AEffect* e, float** in, float** out, int32_t n) {
    if (!out || !out[0] || !out[1]) return;
    Plugin* p = self(e);
    for (int32_t pos = 0; pos < n; pos += kScratch) {
        const int32_t m = std::min<int32_t>(kScratch, n - pos);
        float* tmp[2] = {p->scratch[0], p->scratch[1]};
        processReplacing(e, in, tmp, m);
        for (int32_t i = 0; i < m; ++i) {
            out[0][pos + i] += tmp[0][i];
            out[1][pos + i] += tmp[1][i];
        }
    }
}

void onMidi(Plugin* p, const VstEvents* evs) {
    if (!evs) return;
    for (int32_t i = 0; i < evs->numEvents && p->nMidi < kMaxMidi; ++i) {
        const VstEvent* ev = evs->events[i];
        if (!ev || ev->type != vst::kVstMidiType) continue;
        const auto* me = reinterpret_cast<const VstMidiEvent*>(ev);
        p->midi[p->nMidi++] = {me->deltaFrames, static_cast<uint8_t>(me->midiData[0]),
                               static_cast<uint8_t>(me->midiData[1] & 0x7F), static_cast<uint8_t>(me->midiData[2] & 0x7F)};
    }
}

intptr_t dispatch(Plugin* p, int32_t op, int32_t idx, intptr_t val, void* ptr) {
    const bool validIdx = idx >= 0 && idx < P_COUNT;
    switch (op) {
        case vst::effOpen: return 1;
        case vst::effClose: delete p; return 1;
        case vst::effGetProgram: return 0;
        case vst::effGetProgramName: copyStr(ptr, kPlugName, 24); return 0;
        case vst::effGetPlugCategory: return vst::kPlugCategSynth;
        case vst::effGetEffectName:
        case vst::effGetProductString: copyStr(ptr, kPlugName, 32); return 1;
        case vst::effGetVendorString: copyStr(ptr, kPlugVendor, 32); return 1;
        case vst::effGetVendorVersion: return kPlugVersion;
        case vst::effGetVstVersion: return 2400;
        case vst::effCanBeAutomated: return p->surface.automatable(idx) ? 1 : 0;
        case vst::effGetParamName: copyStr(ptr, validIdx ? PARAM_INFO[idx].name : "", kTextCap); return 0;
        case vst::effGetParamLabel: copyStr(ptr, "", 8); return 0;
        case vst::effGetParamDisplay:
            if (!validIdx) copyStr(ptr, "", kTextCap);
            else if (idx == P_STATUS) copyStr(ptr, statusText(p), kTextCap);
            else copyStr(ptr, p->surface.display(idx), kTextCap);
            return 0;
        case vst::effSetSampleRate:   // MPC OS is fixed at 44.1 kHz; the engine is built for it
        case vst::effSetBlockSize: return 1;
        case vst::effMainsChanged:
            if (val == 0) p->panic.store(true);   // suspended: silence when processing resumes
            return 1;
        case vst::effStopProcess: p->panic.store(true); return 0;
        case vst::effProcessEvents: onMidi(p, static_cast<const VstEvents*>(ptr)); return 1;
        case vst::effCanDo: {
            const char* s = static_cast<const char*>(ptr);
            if (!s) return -1;
            return !std::strcmp(s, "receiveVstEvents") || !std::strcmp(s, "receiveVstMidiEvent") ? 1 : -1;
        }
        case vst::effGetChunk:
            if (!ptr) return 0;
            p->chunk = saveState(p);
            *static_cast<void**>(ptr) = const_cast<char*>(p->chunk.c_str());
            return static_cast<intptr_t>(p->chunk.size() + 1);
        case vst::effSetChunk: {
            if (!ptr || val <= 0) return 0;
            std::string s(static_cast<const char*>(ptr), static_cast<size_t>(val));
            while (!s.empty() && s.back() == '\0') s.pop_back();
            return loadState(p, s) ? 1 : 0;
        }
        default: return 0;
    }
}

intptr_t dispatcher(AEffect* e, int32_t op, int32_t idx, intptr_t val, void* ptr, float /*opt*/) {
    try {
        return dispatch(self(e), op, idx, val, ptr);
    } catch (...) {
        return 0;
    }
}

AEffect* createPlugin(audioMasterCallback master) {
    Plugin* p = new Plugin();
    p->master = master;

    AEffect* e = &p->fx;
    std::memset(e, 0, sizeof(*e));
    e->magic            = vst::kMagic;
    e->dispatcher       = dispatcher;
    e->process          = process;
    e->setParameter     = setParameter;
    e->getParameter     = getParameter;
    e->processReplacing = processReplacing;
    e->numParams        = P_COUNT;
    e->numInputs        = 0;
    e->numOutputs       = 2;
    e->flags            = vst::effFlagsCanReplacing | vst::effFlagsIsSynth | vst::effFlagsProgramChunks;
    e->uniqueID         = kPlugUid;
    e->version          = kPlugVersion;
    e->object           = p;
    return e;
}

} // namespace

// Nothing may throw into MPC: a failed creation reports "no plugin" instead.
extern "C" __attribute__((visibility("default"))) AEffect* VSTPluginMain(audioMasterCallback master) {
    try {
        return createPlugin(master);
    } catch (...) {
        return nullptr;
    }
}
