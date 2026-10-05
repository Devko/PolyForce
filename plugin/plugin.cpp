// PolyForce as a VST2 instrument for MPC OS (Force / MPC standalone).
//
// The engine (dsp/synth.h) behind the touchscreen pages generated from surface/surface.py;
// plugin/surface.* decides what every parameter does, plugin/loader.* loads wavetables in the
// background, and the status line carries a CPU meter so the cost can be read on the device.
//
// Threads: processReplacing runs on one of MPC's audio workers (which one changes between
// calls, instances run concurrently); parameters, display text and chunks come from MPC's UI
// side; the loader has its own worker. Host callbacks are only made from processReplacing.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "vst2.h"
#include "param_ids.h"
#include "patch_map.h"
#include "loader.h"
#include "library.h"
#include "presets.h"
#include "state.h"
#include "surface.h"
#include "trace.h"
#include "cpu_guard.h"
#include "../dsp/tuning.h"
#include "../dsp/notegen.h"
#include "../dsp/synth.h"
#include "../dsp/stages.h"

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

constexpr size_t kTextCap = 128;   // JUCE reads names/display text into 256 bytes; stay well inside
constexpr int kMaxMidi = 512;
constexpr float kSampleRate = 44100.0f;   // MPC OS always runs 44.1 kHz (spec §2.7)
constexpr int kScratch = 512;

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
    Loader              loader{{tableSlotType(), tableSlotType(), tuningSlotType()}};
    Surface             surface{loader};
    std::atomic<bool>   panic{false};
    pf::Synth           synth{kSampleRate};
    pf::NoteGen         gen{kSampleRate};
    std::string         chunk;       // effGetChunk buffer: must outlive the call

    // audio thread only
    float   snapshot[P_COUNT] = {};
    bool    havePatch = false;   // patch / seq below are built from snapshot
    uint32_t seenWrites = 0;     // the surface's write count that snapshot is current for
    Patch   patch;
    SeqPatch seq;
    RawMidi midi[kMaxMidi] = {};
    int     nMidi = 0;
    float   scratch[2][kScratch] = {};
    int     ppqOffset = 0;   // process(): this sub-block starts this many samples into the host's block

    // CPU meter: the audio thread sums its own CPU time against the real-time budget and
    // publishes twice a second; the status line is formatted on the UI thread. The guard
    // sheds release tails when a block costs too much; the meter counts them.
    double           winUs = 0.0, winBudgetUs = 0.0, winPeak = 0.0;
    int              winShed = 0;
    std::atomic<int> shownVoices{0}, shownAvg{0}, shownPeak{0}, shownShed{0};   // percent; tails shed
    int              lastVoices = -1, lastAvg = -1, lastPeak = -1, lastShed = -1;
    CpuGuard         guard;

    const bool       guardOn = [] {   // PF_CPU_GUARD=0 turns it off (the tests: emulated or sanitized
        const char* e = std::getenv("PF_CPU_GUARD");   // blocks are slow, and must stay deterministic)
        return !e || std::strcmp(e, "0") != 0;
    }();
    // The wave view (OSC tab, WAVES): what each oscillator's meters were last set to, and for what. A
    // table's address alone could be a new table in a freed one's place: its name and size count too.
    struct WaveKey {
        const void* table = nullptr;
        size_t name = 0;
        int frames = 0, wave = -1;
        float pos = -1.0f;
        bool operator==(const WaveKey& o) const {
            return table == o.table && name == o.name && frames == o.frames && wave == o.wave && pos == o.pos;
        }
    };
    WaveKey waveKey[2];
    float   waveShown[2][kWaveCols] = {};
    bool    waveValid[2] = {};
    int     waveWait = 0;   // blocks until the view is looked at again

    Plugin() {
        // Loaded tables show in the stepper texts and the browser; a fresh one goes on the
        // Recent list. Runs on the loader's worker.
        loader.setListener([this](int slot, const std::string& key, bool ok) {
            if (slot < 2 && ok && key.compare(0, 8, "builtin:") != 0) tableLibrary().touchRecent(key);   // slot 2: tuning
            surface.refresh();
        });
    }
    ~Plugin() { loader.stop(); }   // its listener uses the surface, destroyed before it
};

Plugin* self(AEffect* e) { return static_cast<Plugin*>(e->object); }

// Copies at most cap - 1 bytes, never cutting a UTF-8 character in half.
void copyStr(void* dst, const std::string& s, size_t cap) {
    if (!dst || cap == 0) return;
    size_t n = std::min(s.size(), cap - 1);
    if (n < s.size())
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;   // s[n] continues a character
    std::memcpy(dst, s.data(), n);
    static_cast<char*>(dst)[n] = 0;
}

std::string statusText(const Plugin* p) {
    const std::string busy = p->surface.busyText();
    if (!busy.empty()) return busy;
    char b[80];
    const int shed = p->shownShed.load();
    if (shed > 0)   // the guard faded release tails in the last half second
        std::snprintf(b, sizeof b, "VOICES %d   CPU %d%%   PEAK %d%%   GUARD %d", p->shownVoices.load(),
                      p->shownAvg.load(), p->shownPeak.load(), shed);
    else
        std::snprintf(b, sizeof b, "VOICES %d   CPU %d%%   PEAK %d%%", p->shownVoices.load(),
                      p->shownAvg.load(), p->shownPeak.load());
    return b;
}

// --- parameters (UI thread) ---------------------------------------------------------------

// Nothing may throw into MPC (a preset load reads a file, allocates, parses).
float getParameter(AEffect* e, int32_t i) {
    try {
        return self(e)->surface.get(i);
    } catch (...) {
        return 0.0f;
    }
}

void setParameter(AEffect* e, int32_t i, float v) {
    try {
        Surface& s = self(e)->surface;
        const bool traced = i >= 0 && i < P_COUNT && tracing();
        const float before = traced ? s.get(i) : 0.0f;   // what MPC last read back
        s.set(i, v);
        if (traced)
            trace("%p set %3d %-18s %.4f  read %.4f -> %.4f  \"%s\"", static_cast<void*>(e), static_cast<int>(i),
                  PARAM_INFO[i].key, static_cast<double>(v), static_cast<double>(before), static_cast<double>(s.get(i)),
                  s.display(i).c_str());
    } catch (...) {
    }
}

// --- audio thread -------------------------------------------------------------------------

void handleMidi(Plugin* p, const RawMidi& m) {
    pf::Synth& s = p->synth;
    switch (m.status & 0xF0) {
        case 0x90:   // keys go through the arpeggiator / sequencer (straight on when it's off)
            if (m.d2) p->gen.keyOn(m.d1, m.d2, s);
            else p->gen.keyOff(m.d1, s);
            break;
        case 0x80: p->gen.keyOff(m.d1, s); break;
        case 0xB0:
            if (m.d1 == 64) p->gen.pedal(m.d2 >= 64, s);   // sustains notes, or holds the arp's keys
            else if (m.d1 == 120) {
                p->gen.panic(s);
                s.reset();
            } else if (m.d1 == 123) {
                p->gen.panic(s);
                s.allNotesOff();
            } else {
                s.controller(m.d1, m.d2);   // mod wheel, breath, expression (matrix sources)
            }
            break;
        case 0xD0: s.aftertouch(static_cast<float>(m.d1) / 127.0f); break;
        case 0xA0: s.polyAftertouch(m.d1, static_cast<float>(m.d2) / 127.0f); break;
        case 0xE0:   // -1..1; the patch's bend-up/down ranges turn it into semitones
            s.pitchBend(static_cast<float>((m.d2 << 7 | m.d1) - 8192) / 8192.0f);
            break;
        default: break;
    }
}

// Renders [pos, to), stopping wherever the arpeggiator / sequencer has an event due.
void renderTo(Plugin* p, float* L, float* R, int& pos, int to) {
    while (pos < to) {
        const int step = p->gen.untilNext(to - pos);
        p->synth.render(L + pos, R + pos, step);
        pos += step;
        const int was = p->gen.currentStep();
        p->gen.advance(step, p->synth);
        if (p->gen.currentStep() != was) {   // the Seq lane moves at the step, mid-block
            float shapes[4];
            p->gen.shapeValues(shapes);
            p->synth.setSequencerSources(p->gen.seqValue(), shapes);
        }
    }
}

// The WAVES page: each oscillator's current frame as kWaveCols meter values, each one of the strip's 128
// levels, when a table, wave or position changed (a few thousand reads then, nothing otherwise). MPC
// doesn't tell a plugin which page shows, so it is kept up to date on every page, at most every
// kWaveEveryBlocks (~46 ms): a sweep or an automated position pushes ~20 views a second, not one a block.
// MPC hears the columns that moved via notify().
constexpr int kWaveEveryBlocks = 16;
static_assert(P_O2_WV01 == P_O1_WV01 + kWaveCols, "the wave view's parameters: two runs of kWaveCols");
void updateWaveView(Plugin* p) {
    if (p->waveWait > 0) {
        --p->waveWait;
        return;
    }
    p->waveWait = kWaveEveryBlocks - 1;
    for (int o = 0; o < 2; ++o) {
        const Wavetable* t = p->synth.oscTable(o);
        const Plugin::WaveKey key{t, t ? std::hash<std::string>{}(t->name) : 0, t ? t->frames : 0,
                                  p->patch.osc[o].wave, p->patch.osc[o].pos};
        if (p->waveValid[o] && key == p->waveKey[o]) continue;
        float cols[kWaveCols];
        p->synth.waveView(o, cols, kWaveCols);
        const int first = o ? P_O2_WV01 : P_O1_WV01;
        for (int c = 0; c < kWaveCols; ++c) {
            const float v = std::round((cols[c] + 1.0f) * 63.5f) / 127.0f;
            if (p->waveValid[o] && v == p->waveShown[o][c]) continue;
            p->waveShown[o][c] = v;
            p->surface.setValue(first + c, v);
        }
        p->waveKey[o] = key;
        p->waveValid[o] = true;
    }
}

void runBlock(Plugin* p, float* L, float* R, int n) {
    // The sound only changes when a parameter or a loaded table does: then rebuild the patch
    // (a few hundred value conversions) and hand it to the engine; else keep both as they are.
    // Looked at only when something was written since the last look: copying and comparing every
    // value is a few thousand instructions, every block, for nothing most of the time.
    bool changed = false;
    const uint32_t writes = p->surface.writes();   // before the snapshot: a later write shows next block
    if (!p->havePatch || writes != p->seenWrites) {
        float fresh[P_COUNT];
        if (p->surface.snapshot(fresh)) {   // mid-preset: false, look again next block
            p->seenWrites = writes;
            if (!p->havePatch || std::memcmp(fresh, p->snapshot, sizeof fresh) != 0) {
                std::memcpy(p->snapshot, fresh, sizeof fresh);
                p->patch = patchFromParams(p->snapshot);
                p->seq = seqFromParams(p->snapshot);
                p->havePatch = changed = true;
            }
        }
    }
    for (int o = 0; o < 2; ++o) {   // read once per block: valid until blockDone() below
        const auto* t = static_cast<const Wavetable*>(p->loader.live(o));
        changed = changed || t != p->patch.osc[o].table;
        p->patch.osc[o].table = t;
    }
    const auto* tuning = static_cast<const Tuning*>(p->loader.live(2));
    const float* pitches = tuning ? tuning->pitch : nullptr;
    changed = changed || pitches != p->patch.tuning;
    p->patch.tuning = pitches;
    if (changed) p->synth.setPatch(p->patch);
    updateWaveView(p);
    if (p->panic.exchange(false)) {
        p->gen.panic(p->synth);
        p->synth.reset();
    }
    // MPC's tempo and bar position: synced LFOs and the sequencers follow them.
    double bpm = 120.0, beats = 0.0;
    bool playing = false, valid = false;
    if (p->master) {
        const intptr_t r = p->master(&p->fx, vst::audioMasterGetTime, 0,
                                     vst::kVstTempoValid | vst::kVstPpqPosValid, nullptr, 0.0f);
        if (const VstTimeInfo* t = reinterpret_cast<const VstTimeInfo*>(r)) {
            // A NaN or absurd value would stall or spin the sequencer clock: ignore it.
            if ((t->flags & vst::kVstTempoValid) && std::isfinite(t->tempo) && t->tempo >= 1.0 && t->tempo <= 1000.0) bpm = t->tempo;
            valid = (t->flags & vst::kVstPpqPosValid) != 0 && std::isfinite(t->ppqPos) && std::fabs(t->ppqPos) < 1e9;
            beats = valid ? t->ppqPos + p->ppqOffset * bpm / 60.0 / static_cast<double>(kSampleRate) : 0.0;
            playing = (t->flags & vst::kVstTransportPlaying) != 0;
        }
    }
    p->synth.setTransport(bpm, beats, playing, valid);
    p->gen.setTransport(bpm, beats, playing, valid);
    p->gen.setPatch(p->seq, p->synth);   // every block: it also follows the keys and the clock
    float shapes[4];
    p->gen.shapeValues(shapes);
    p->synth.setSequencerSources(p->gen.seqValue(), shapes);
    p->gen.advance(0, p->synth);   // a step due exactly at the block start

    // Events in time order (insertion sort: no allocation; MPC already sends them sorted),
    // each one applied at its own sample.
    for (int i = 1; i < p->nMidi; ++i)
        for (int j = i; j > 0 && p->midi[j].delta < p->midi[j - 1].delta; --j) std::swap(p->midi[j], p->midi[j - 1]);
    int pos = 0;
    for (int i = 0; i < p->nMidi; ++i) {
        renderTo(p, L, R, pos, std::clamp(static_cast<int>(p->midi[i].delta), 0, n));
        handleMidi(p, p->midi[i]);
    }
    renderTo(p, L, R, pos, n);
    p->nMidi = 0;

    // Steps the recorder wrote become parameter values (atomics; MPC hears via notify()).
    NoteGen::Recorded rec;
    while (p->gen.takeRecorded(rec)) {
        const int k = std::clamp(rec.step, 0, kSeqSteps - 1);
        p->surface.setValue(P_S1_NOTE + k, paramNorm(P_S1_NOTE + k, static_cast<float>(rec.note)));
        p->surface.setValue(P_S1_VEL + k, paramNorm(P_S1_VEL + k, static_cast<float>(rec.vel)));
    }
}

// us: the whole call; engineUs: runBlock alone, what the guard judges (MPC's own callbacks
// from notify() are not ours to shed for).
void meter(Plugin* p, double us, double engineUs, int n) {
    const double budget = static_cast<double>(n) * 1e6 / kSampleRate;
    if (p->guardOn)
        if (const int k = p->guard.afterBlock(engineUs, budget)) p->winShed += p->synth.shedTails(k);
    p->winUs += us;
    p->winBudgetUs += budget;
    p->winPeak = std::max(p->winPeak, us / budget);
    if (p->winBudgetUs < 500000.0) return;

    const int avg = static_cast<int>(std::lround(100.0 * p->winUs / p->winBudgetUs));
    const int peak = static_cast<int>(std::lround(100.0 * p->winPeak));
    const int voices = p->synth.activeVoices(), shed = p->winShed;
    p->winUs = p->winBudgetUs = p->winPeak = 0.0;
    p->winShed = 0;
    p->shownAvg.store(avg);
    p->shownPeak.store(peak);
    p->shownVoices.store(voices);
    p->shownShed.store(shed);
    const bool expired = p->surface.statusExpired();   // a MISSING note ran its time: show the meter again
    if (expired || avg != p->lastAvg || peak != p->lastPeak || voices != p->lastVoices || shed != p->lastShed) {
        p->lastAvg = avg;
        p->lastPeak = peak;
        p->lastVoices = voices;
        p->lastShed = shed;
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
    p->loader.blockStart();
    try {
        runBlock(p, out[0], out[1], n);
    } catch (...) {   // nothing may throw into MPC: an escaping exception ends the whole process
        std::memset(out[0], 0, sizeof(float) * static_cast<size_t>(n));
        std::memset(out[1], 0, sizeof(float) * static_cast<size_t>(n));
    }
    const double t1 = threadCpuUs();
    p->loader.blockDone();   // the tables read at block start are no longer in use
    p->surface.notify(hostAutomate, hostUpdate, p);
    meter(p, threadCpuUs() - t0, t1 - t0, n);
}

// Legacy accumulating entry point (MPC uses processReplacing): sub-blocks of kScratch, each
// with its own events and its own place on the host's timeline.
void process(AEffect* e, float** in, float** out, int32_t n) {
    if (!out || !out[0] || !out[1]) return;
    Plugin* p = self(e);
    RawMidi all[kMaxMidi];
    const int nAll = p->nMidi;
    std::copy(p->midi, p->midi + nAll, all);
    for (int32_t pos = 0; pos < n; pos += kScratch) {
        const int32_t m = std::min<int32_t>(kScratch, n - pos);
        const bool last = pos + m >= n;
        p->nMidi = 0;
        for (int i = 0; i < nAll; ++i) {
            const int32_t d = std::max<int32_t>(all[i].delta, 0);
            if ((d >= pos && d < pos + m) || (last && d >= pos + m)) {
                p->midi[p->nMidi] = all[i];
                p->midi[p->nMidi++].delta = d - pos;
            }
        }
        p->ppqOffset = pos;
        float* tmp[2] = {p->scratch[0], p->scratch[1]};
        processReplacing(e, in, tmp, m);
        for (int32_t i = 0; i < m; ++i) {
            out[0][pos + i] += tmp[0][i];
            out[1][pos + i] += tmp[1][i];
        }
    }
    p->ppqOffset = 0;
}

// The last kEndReserve slots only take what ends notes (note-off, pedal, all notes/sound off):
// a flood of note-ons or controllers must not leave a note stuck by crowding out its note-off.
constexpr int kEndReserve = 64;

void onMidi(Plugin* p, const VstEvents* evs) {
    if (!evs) return;
    for (int32_t i = 0; i < evs->numEvents && p->nMidi < kMaxMidi; ++i) {
        const VstEvent* ev = evs->events[i];
        if (!ev || ev->type != vst::kVstMidiType) continue;
        const auto* me = reinterpret_cast<const VstMidiEvent*>(ev);
        const uint8_t st = static_cast<uint8_t>(me->midiData[0]);
        const uint8_t d1 = static_cast<uint8_t>(me->midiData[1] & 0x7F), d2 = static_cast<uint8_t>(me->midiData[2] & 0x7F);
        const int type = st & 0xF0;
        const bool ends = type == 0x80 || (type == 0x90 && d2 == 0) || (type == 0xB0 && (d1 == 64 || d1 == 120 || d1 == 123));
        if (!ends && p->nMidi >= kMaxMidi - kEndReserve) continue;
        p->midi[p->nMidi++] = {me->deltaFrames, st, d1, d2};
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
            p->chunk = saveState(p->surface, false);
            *static_cast<void**>(ptr) = const_cast<char*>(p->chunk.c_str());
            return static_cast<intptr_t>(p->chunk.size() + 1);
        case vst::effSetChunk: {
            if (!ptr || val <= 0 || val > (1 << 20)) return 0;   // a state is ~10 KB; more is not ours
            std::string s(static_cast<const char*>(ptr), static_cast<size_t>(val));
            while (!s.empty() && s.back() == '\0') s.pop_back();
            return loadState(p->surface, s, false) ? 1 : 0;
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

#ifdef PF_STAGE_TIMING
// The profiling build only (make arm-bench-stages): the engine's time per render pass since
// the last call, in microseconds, with the passes' names; tools/bench.cpp reads it with dlsym.
// Returns the number of passes written.
extern "C" __attribute__((visibility("default"))) int PolyForceStageTimes(double* us, const char** names, int max) {
    if (!us || !names || max < pf::STG_COUNT) return 0;   // all passes or none (each read starts them over)
    for (int i = 0; i < pf::STG_COUNT; ++i) {
        us[i] = static_cast<double>(pf::g_stageNs[i]) * 1e-3;
        names[i] = pf::kStageNames[i];
        pf::g_stageNs[i] = 0;
    }
    return pf::STG_COUNT;
}
#endif
