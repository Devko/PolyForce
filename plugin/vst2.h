#pragma once
// The slice of the VST2 ABI that MPC OS's JUCE host uses, written out by hand
// (no Steinberg SDK). Layout and opcode values follow sd88me/mpc-vst-plugins
// wrapper/vst2_wrap.c (MIT, Copyright (c) 2026 sd88me), which is device-verified
// on a Force. On 32-bit ARM intptr_t is 4 bytes.
#include <cstdint>

extern "C" {

struct AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect*, int32_t, int32_t, intptr_t, void*, float);

struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect*, int32_t, int32_t, intptr_t, void*, float);
    void (*process)(AEffect*, float**, float**, int32_t);
    void (*setParameter)(AEffect*, int32_t, float);
    float (*getParameter)(AEffect*, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect*, float**, float**, int32_t);
    void (*processDoubleReplacing)(AEffect*, double**, double**, int32_t);
    char future[56];
};

struct VstEvent { int32_t type, byteSize, deltaFrames, flags; char data[16]; };
struct VstMidiEvent {
    int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset;
    unsigned char midiData[4];
    char detune, noteOffVelocity, reserved1, reserved2;
};
struct VstEvents { int32_t numEvents; intptr_t reserved; VstEvent* events[2]; };  // events[] is variable length

struct VstTimeInfo {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
};

} // extern "C"

namespace vst {

constexpr int32_t kMagic = 0x56737450;   // 'VstP' (the forum snippet's value is wrong)

enum : int32_t {
    effOpen = 0, effClose = 1, effGetProgram = 3, effGetProgramName = 5, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effGetChunk = 23,
    effSetChunk = 24, effProcessEvents = 25, effCanBeAutomated = 26, effGetPlugCategory = 35,
    effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48,
    effGetVendorVersion = 49, effCanDo = 51, effGetVstVersion = 58, effStopProcess = 72,
};

enum : int32_t {
    audioMasterAutomate = 0, audioMasterGetTime = 7, audioMasterUpdateDisplay = 42,
};

enum : int32_t {
    kVstTransportPlaying = 1 << 1, kVstCycleActive = 1 << 2, kVstPpqPosValid = 1 << 9,
    kVstTempoValid = 1 << 10, kVstCyclePosValid = 1 << 12,
};

enum : int32_t {
    effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5, effFlagsIsSynth = 1 << 8,
};

constexpr int32_t kVstMidiType = 1;
constexpr int32_t kPlugCategSynth = 2;

constexpr int32_t fourcc(const char (&s)[5]) {
    return (int32_t(uint8_t(s[0])) << 24) | (int32_t(uint8_t(s[1])) << 16) |
           (int32_t(uint8_t(s[2])) << 8) | int32_t(uint8_t(s[3]));
}

} // namespace vst
