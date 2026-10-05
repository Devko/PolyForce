#pragma once
// Band-limited wavetables. A table is a stack of single-cycle frames (the "position" axis);
// every frame is stored as kMipLevels pre-filtered copies, level k keeping harmonics
// 1..(1024 >> k). An oscillator picks the level whose top harmonic stays clear of
// aliasing for the pitch it plays, so the inner loop is a plain lookup with no filtering.
//
// Level k is stored at its own length, 1 << kMipBits[k] samples (+1 guard sample): linear
// interpolation needs ~8 samples per cycle of the top harmonic, so the high levels (few
// harmonics) are short. 9,227 floats per frame instead of 11 x 2,049 = 22,539.
#include <cstddef>
#include <string>
#include <vector>

namespace pf {

constexpr int kTableBits   = 11;
constexpr int kTableSize   = 1 << kTableBits;   // samples of level 0 (2048)
constexpr int kMaxHarmonic = kTableSize / 2;    // 1024
constexpr int kMipLevels   = 11;                // 1024, 512, ... 1 harmonics

// Samples of level k = 1 << kMipBits[k] = clamp(8 * (1024 >> k), 256, 2048).
constexpr int kMipBits[kMipLevels] = {11, 11, 11, 10, 9, 8, 8, 8, 8, 8, 8};

constexpr int mipLength(int k) { return 1 << kMipBits[k]; }
constexpr int mipOffset(int k) { return k == 0 ? 0 : mipOffset(k - 1) + mipLength(k - 1) + 1; }
constexpr int kFrameStride = mipOffset(kMipLevels - 1) + mipLength(kMipLevels - 1) + 1;
static_assert(kFrameStride == 9227, "mip layout changed: update the comment above");

struct Wavetable {
    std::string name;
    int frames = 0;
    std::vector<float> data;   // [frame][level][mipLength(level) + 1]

    // Level `mip` of `frame`: mipLength(mip) samples plus a guard sample (= sample 0).
    const float* get(int frame, int mip) const {
        return data.data() + static_cast<size_t>(frame) * kFrameStride + static_cast<size_t>(mipOffset(mip));
    }
    size_t bytes() const { return data.size() * sizeof(float); }
};

// The library's built-in tables, all computed (no files): Classic, PWM, Sync, Formant and the
// families after them (analog, FM, digital, vocal, acoustic; see wavetable.cpp), in browser
// order. Classic is built once per process on first use and kept (it is every table slot's
// fallback and the engine's default); the others are built on demand by buildBuiltin(), at
// load time (the loader thread caches them like imported files).
int builtinCount();
const char* builtinName(int i);                   // "" out of range
int builtinIndex(const std::string& name);        // -1 if there is no such built-in
const Wavetable& classicBuiltin();                // built-in 0
bool buildBuiltin(int i, Wavetable& out);         // false out of range

// The classic oscillator shapes as one-frame tables, plus the pulse table (32 frames, width
// 50% -> 3%; position = width). Same lifetime as classicBuiltin().
enum ClassicWave : int { CW_SINE, CW_TRIANGLE, CW_SAW, CW_SQUARE, CW_PULSE, CW_COUNT };
const Wavetable& classicTable(int wave);

// Loads a wavetable WAV in the Serum layout: frames of N samples back to back (N from the
// 'clm ' chunk "<!>2048 ...", else 2048), mono or first channel, float32 or 16/24/32-bit
// PCM, at most kMaxFrames frames. The whole table is normalised to peak 1 with ONE gain, so
// level changes the author put across the frames (one-shot tables fade out) survive.
// Load time, not real time: false + a reason in *err for anything it can't use.
constexpr int kMaxFrames = 256;
bool loadWavetable(const std::string& path, Wavetable& out, std::string* err = nullptr);

// Highest harmonic h allowed at phase increment `inc` (cycles/sample): we let partials go
// above Nyquist as long as their alias folds back above ~18 kHz (h * inc <= 0.59 at 44.1 kHz),
// which keeps the top octave bright instead of dulling every note to the safe limit.
constexpr float kAliasLimit = 0.59f;

inline int mipFor(float inc) {
    const float allowed = kAliasLimit / inc;
    int k = 0;
    while (k < kMipLevels - 1 && static_cast<float>(kMaxHarmonic >> k) > allowed) ++k;
    return k;
}

} // namespace pf
