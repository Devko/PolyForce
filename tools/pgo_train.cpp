// The profile-guided build's trainer (make arm-plugin with PGO): an instrumented copy of the
// plugin is linked in and plays a spread of patches through VSTPluginMain under qemu-arm, the
// way MPC drives it. Every filter type in filter 1 (filter 2 another one), the three engines,
// unison 1 / 3 / 8, a busy matrix, more notes than voices (steals), a release, both routings,
// subs, noise and the classic waves. The profile only steers the compiler (which paths are hot);
// what the trainer leaves out is still optimised as usual (-fprofile-partial-training).
#include "../dsp/mod.h"
#include "../dsp/synth.h"
#include "../plugin/vst2.h"
#include "param_ids.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

extern "C" AEffect* VSTPluginMain(audioMasterCallback);

namespace {

constexpr int kFilterTypes = pf::F_VOWEL + 1;   // Off .. Vowel
VstTimeInfo g_time{};

intptr_t master(AEffect*, int32_t op, int32_t, intptr_t, void*, float) {
    if (op == 1) return 2400;   // audioMasterVersion
    if (op == vst::audioMasterGetTime) return reinterpret_cast<intptr_t>(&g_time);
    return 0;
}

float norm(int id, float v) {
    const pf::ParamSpec& s = pf::PARAM_SPECS[id];
    const float n = s.curve == pf::Curve::Log ? std::log(v / s.lo) / std::log(s.hi / s.lo)
                                              : (s.hi > s.lo ? (v - s.lo) / (s.hi - s.lo) : 0.0f);
    return std::clamp(n, 0.0f, 1.0f);
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

int main() {
    g_time.sampleRate = 44100.0;
    g_time.tempo = 120.0;
    g_time.flags = vst::kVstTempoValid | vst::kVstPpqPosValid;
    std::vector<float> L(128), R(128);
    float* out[2] = {L.data(), R.data()};
    int patches = 0;
    for (int f1 = 0; f1 < kFilterTypes; ++f1)
        for (int eng = 0; eng <= 2; ++eng)
            for (int uni : {1, 3, 8})
                for (int busy = 0; busy <= 1; ++busy) {
                    if (busy && uni != 3) continue;
                    AEffect* e = VSTPluginMain(master);
                    e->dispatcher(e, vst::effOpen, 0, 0, nullptr, 0.0f);
                    e->dispatcher(e, vst::effSetSampleRate, 0, 0, nullptr, 44100.0f);
                    e->dispatcher(e, vst::effSetBlockSize, 0, 128, nullptr, 0.0f);
                    e->dispatcher(e, vst::effMainsChanged, 0, 1, nullptr, 0.0f);
                    auto set = [e](int id, float v) { e->setParameter(e, id, norm(id, v)); };
                    set(pf::P_VOICES, 5.0f);
                    set(pf::P_O1_LEVEL, 0.8f);
                    set(pf::P_O2_LEVEL, 0.6f);
                    set(pf::P_F1_TYPE, static_cast<float>(f1));
                    set(pf::P_F1_DRIVE, (f1 % 2) ? 0.5f : 0.0f);
                    set(pf::P_F1_RES, 0.6f);
                    set(pf::P_F2_TYPE, static_cast<float>((f1 + 3) % kFilterTypes));
                    set(pf::P_F2_CUT, 3000.0f);
                    set(pf::P_ROUTING, static_cast<float>(f1 % 2));
                    set(pf::P_O2_ROUTE, static_cast<float>(f1 % 4));
                    set(pf::P_E2_POS, 0.3f);
                    set(pf::P_ENGINE, static_cast<float>(eng));
                    set(pf::P_O1_SUB_LEVEL, (f1 % 3) ? 0.0f : 0.5f);
                    set(pf::P_NOISE_LEVEL, (f1 % 5) ? 0.0f : 0.3f);
                    set(pf::P_O1_UNI, static_cast<float>(uni));
                    set(pf::P_O2_UNI, static_cast<float>(uni));
                    set(pf::P_O2_WAVE, static_cast<float>(f1 % 7));
                    if (busy) {
                        struct S { int src, tgt; float amt; int mod; };
                        const S slots[8] = {{pf::MS_LFO1, pf::MT_PITCH, 0.05f, pf::MM_NONE},
                                            {pf::MS_LFO2, pf::MT_O1_POS, 0.4f, pf::MM_NONE},
                                            {pf::MS_LFO1, pf::MT_F1_CUT, 0.3f, pf::MM_NONE},
                                            {pf::MS_LFO2, pf::MT_F2_CUT, -0.3f, pf::MM_NONE},
                                            {pf::MS_ENV2, pf::MT_O2_POS, 0.5f, pf::MM_NONE},
                                            {pf::MS_VELOCITY, pf::MT_CUT, 0.2f, pf::MM_SLEW},
                                            {pf::MS_NOTE, pf::MT_O1_PAN, 0.5f, pf::MM_SAMPLE_HOLD},
                                            {pf::MS_LFO1, pf::MT_F1_DRIVE, 0.5f, pf::MM_NONE}};
                        const int stride = pf::P_M2_SRC - pf::P_M1_SRC;
                        for (int k = 0; k < 8; ++k) {
                            set(pf::P_M1_SRC + k * stride, static_cast<float>(slots[k].src));
                            set(pf::P_M1_T1 + k * stride, static_cast<float>(slots[k].tgt));
                            set(pf::P_M1_A1 + k * stride, slots[k].amt);
                            set(pf::P_M1_MOD + k * stride, static_cast<float>(slots[k].mod));
                        }
                        set(pf::P_L1_RATE, 5.0f);
                        set(pf::P_L2_RATE, 0.3f);
                    }
                    for (int b = 0; b < 40; ++b) {
                        if (b < 7) midi(e, 0x90, static_cast<uint8_t>(40 + b * 5), static_cast<uint8_t>(60 + b * 9));   // 7 notes > 5 voices: steals
                        if (b == 30) {
                            midi(e, 0x80, 40 + 30, 0);
                            midi(e, 0x80, 40 + 25, 0);
                        }
                        e->processReplacing(e, nullptr, out, 128);
                    }
                    e->dispatcher(e, vst::effClose, 0, 0, nullptr, 0.0f);
                    ++patches;
                }
    std::printf("pgo training: %d patches\n", patches);
    return 0;
}
