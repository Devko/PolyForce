// pfloud: the factory presets' levels. dlopen()s the built plugin like MPC, plays every preset of
// presets/Factory on a phrase that suits it and measures its loudness (ITU-R BS.1770: K-weighted,
// L and R, the loudest window: 3 s, or 400 ms for a one-shot) and its sample peak. `make loudness`.
//
//   pfloud <plugin.so> [-g dB] [-c category]
//
// -g: report the levels as if every preset were dB louder (what a level shift would do), and flag
// the ones whose peak would pass the ceiling. -c: only one category.
//
// Phrases (velocity 100, 120 BPM): Bass a line of four 1 s notes around C1; Lead a line around C4;
// a one-shot (amp sustain under 5%) a hit every 0.5 s; Arp & Seq a held chord with the transport
// running; everything else a held four-note chord (C3 E3 G3 C4). Hermetic: the plugin reads no user
// folders and saves nothing.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../plugin/vst2.h"
#include "factory_presets.h"
#include "param_ids.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kBlock = 128;
constexpr double kSr = 44100.0;
constexpr double kCeilingDb = -1.0;   // a preset's peak at most this after a level shift

VstTimeInfo g_time{};

intptr_t master(AEffect*, int32_t op, int32_t, intptr_t, void*, float) {
    if (op == 1) return 2400;   // audioMasterVersion
    if (op == vst::audioMasterGetTime) return reinterpret_cast<intptr_t>(&g_time);
    return 0;
}

// A BS.1770 K-weighting stage: the shelf and the high-pass, as libebur128 derives them for any rate.
struct Biquad {
    double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
    double run(double x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};
Biquad shelf() {
    const double f0 = 1681.974450955533, g = 3.999843853973347, q = 0.7071752369554196;
    const double k = std::tan(M_PI * f0 / kSr), vh = std::pow(10.0, g / 20.0), vb = std::pow(vh, 0.4996667741545416);
    const double a0 = 1.0 + k / q + k * k;
    return {(vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0,
            2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0};
}
Biquad highpass() {
    const double f0 = 38.13547087602444, q = 0.5003270373238773;
    const double k = std::tan(M_PI * f0 / kSr), a0 = 1.0 + k / q + k * k;
    return {1.0, -2.0, 1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0};
}

// The loudest window of `windowS`, in LUFS (hop 100 ms), over the K-weighted stereo signal.
double loudest(const std::vector<float>& L, const std::vector<float>& R, double windowS) {
    Biquad s[2] = {shelf(), shelf()}, h[2] = {highpass(), highpass()};
    std::vector<double> power(L.size());
    for (size_t i = 0; i < L.size(); ++i) {
        const double l = h[0].run(s[0].run(L[i])), r = h[1].run(s[1].run(R[i]));
        power[i] = l * l + r * r;
    }
    const size_t win = static_cast<size_t>(windowS * kSr), hop = static_cast<size_t>(0.1 * kSr);
    double best = 0.0;
    for (size_t at = 0; at + win <= power.size(); at += hop) {
        double sum = 0.0;
        for (size_t i = at; i < at + win; ++i) sum += power[i];
        best = std::max(best, sum / static_cast<double>(win));
    }
    return best > 0.0 ? -0.691 + 10.0 * std::log10(best) : -99.0;
}

float valueOf(const char* text, const char* key, float fallback) {
    const std::string t = text, k = std::string("\n") + key + "=";
    const size_t at = t.find(k);
    return at == std::string::npos ? fallback : std::strtof(t.c_str() + at + k.size(), nullptr);
}

struct Note { double at, len; int key; };

std::vector<Note> phrase(const std::string& category, bool oneShot) {
    std::vector<Note> out;
    if (oneShot) {
        const int key = category == "Drum & Perc" ? 48 : 60;
        for (int k = 0; k < 8; ++k) out.push_back({0.5 * k, 0.25, key});
    } else if (category == "Bass") {
        const int keys[] = {36, 43, 39, 41};
        for (int k = 0; k < 4; ++k) out.push_back({1.0 * k, 1.0, keys[k]});
    } else if (category == "Lead") {
        const int keys[] = {60, 67, 63, 65};
        for (int k = 0; k < 4; ++k) out.push_back({1.0 * k, 1.0, keys[k]});
    } else {
        for (int key : {48, 52, 55, 60}) out.push_back({0.0, 4.0, key});
    }
    return out;
}

void midi(AEffect* e, uint8_t st, uint8_t d1, uint8_t d2, int delta) {
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

std::string display(AEffect* e, int id) {
    char b[256] = {};
    e->dispatcher(e, vst::effGetParamDisplay, id, 0, b, 0.0f);
    return b;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: pfloud <plugin.so> [-g dB] [-c category]\n");
        return 2;
    }
    double shift = 0.0;
    std::string only;
    for (int a = 2; a + 1 < argc; a += 2) {
        if (!std::strcmp(argv[a], "-g")) shift = std::atof(argv[a + 1]);
        else if (!std::strcmp(argv[a], "-c")) only = argv[a + 1];
    }
    char tmpl[] = "/tmp/pfloud.XXXXXX";
    const char* dir = mkdtemp(tmpl);
    for (const char* v : {"PF_DATA_DIR", "PF_TABLE_ROOTS", "PF_PRESET_ROOTS", "PF_TUNING_ROOTS"}) setenv(v, dir, 1);
    setenv("PF_CPU_GUARD", "0", 1);
    void* so = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!so) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    auto entry = reinterpret_cast<AEffect* (*)(audioMasterCallback)>(dlsym(so, "VSTPluginMain"));
    g_time.sampleRate = kSr;
    g_time.tempo = 120.0;
    g_time.timeSigNumerator = g_time.timeSigDenominator = 4;

    std::printf("%-14s %-20s %6s %5s %7s %7s%s\n", "category", "preset", "vol dB", "shot", "LUFS", "peak", shift ? "  (shifted)" : "");
    double sumL = 0, minL = 99, maxL = -99, maxPk = -99;
    int n = 0, over = 0;
    std::vector<float> L, R;
    for (int p = 0; p < pf::kNumFactoryPresets; ++p) {
        const pf::FactoryPreset& fp = pf::kFactoryPresets[p];
        if (!only.empty() && only != fp.category) continue;
        const bool oneShot = valueOf(fp.text, "e1_s", 0.8f) < 0.05f;
        const bool transport = std::string(fp.category) == "Arp & Seq";
        AEffect* e = entry(master);
        e->dispatcher(e, vst::effOpen, 0, 0, nullptr, 0.0f);
        e->dispatcher(e, vst::effSetChunk, 0, static_cast<intptr_t>(std::strlen(fp.text)), const_cast<char*>(fp.text), 0.0f);
        float bl[kBlock], br[kBlock];
        float* out[2] = {bl, br};
        g_time.flags = vst::kVstTempoValid | vst::kVstPpqPosValid;
        g_time.ppqPos = 0;
        // The tables load on the plugin's own thread: play silence until both are in.
        for (int t = 0; t < 2000; ++t) {
            e->processReplacing(e, nullptr, out, kBlock);
            if (display(e, pf::P_O1_TABLE).rfind("LOADING", 0) != 0 && display(e, pf::P_O2_TABLE).rfind("LOADING", 0) != 0)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (transport) g_time.flags |= vst::kVstTransportPlaying;
        const std::vector<Note> notes = phrase(fp.category, oneShot);
        const double lengthS = (transport ? 6.0 : 4.0) + 1.0;
        const size_t blocks = static_cast<size_t>(lengthS * kSr / kBlock);
        L.assign(blocks * kBlock, 0.0f);
        R.assign(blocks * kBlock, 0.0f);
        for (size_t b = 0; b < blocks; ++b) {
            const double t0 = static_cast<double>(b * kBlock) / kSr, t1 = t0 + kBlock / kSr;
            for (const Note& nt : notes) {
                const double on = nt.at, off = transport ? lengthS - 1.0 : nt.at + nt.len;
                if (on >= t0 && on < t1) midi(e, 0x90, static_cast<uint8_t>(nt.key), 100, static_cast<int>((on - t0) * kSr));
                if (off >= t0 && off < t1) midi(e, 0x80, static_cast<uint8_t>(nt.key), 0, static_cast<int>((off - t0) * kSr));
            }
            float* o[2] = {&L[b * kBlock], &R[b * kBlock]};
            e->processReplacing(e, nullptr, o, kBlock);
            if (g_time.flags & vst::kVstTransportPlaying) g_time.ppqPos += kBlock / kSr * g_time.tempo / 60.0;
        }
        e->dispatcher(e, vst::effClose, 0, 0, nullptr, 0.0f);

        const double gain = std::pow(10.0, shift / 20.0);
        float pk = 0.0f;
        for (size_t i = 0; i < L.size(); ++i) pk = std::max(pk, std::max(std::fabs(L[i]), std::fabs(R[i])));
        const double lufs = loudest(L, R, oneShot ? 0.4 : 3.0) + shift;
        const double peakDb = 20.0 * std::log10(std::max(1e-9, static_cast<double>(pk) * gain));
        const bool hot = peakDb > kCeilingDb;
        std::printf("%-14s %-20s %6.1f %5s %7.1f %7.1f%s\n", fp.category, fp.name, valueOf(fp.text, "volume", -6.0f),
                    oneShot ? "yes" : "", lufs, peakDb, hot ? "  PEAK" : "");
        sumL += lufs;
        minL = std::min(minL, lufs);
        maxL = std::max(maxL, lufs);
        maxPk = std::max(maxPk, peakDb);
        over += hot ? 1 : 0;
        ++n;
    }
    std::printf("\n%d presets: loudness %.1f LUFS mean (%.1f .. %.1f), highest peak %.1f dBFS, %d over %.0f dBFS\n", n,
                n ? sumL / n : 0.0, minL, maxL, maxPk, over, kCeilingDb);
    return 0;
}
