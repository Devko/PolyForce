#include "wavetable.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>

namespace pf {
namespace {

using cd = std::complex<double>;
constexpr double kPi = 3.14159265358979323846;

// In-place iterative radix-2 FFT, load-time only. The inverse includes the 1/N. The
// butterfly multiplies by hand: std::complex's operator* calls __muldc3 (NaN/inf fix-ups)
// unless -ffast-math, which is several times slower on ARM.
void fft(std::vector<cd>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2.0 * kPi / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
        const double wr = std::cos(ang), wi = std::sin(ang);
        for (size_t i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (size_t k = 0; k < len / 2; ++k) {
                const cd u = a[i + k];
                const cd x = a[i + k + len / 2];
                const cd v(x.real() * cr - x.imag() * ci, x.real() * ci + x.imag() * cr);
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                const double nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
    if (inverse)
        for (auto& x : a) x /= static_cast<double>(n);
}

// A frame's spectrum: cosine (a) and sine (b) amplitude of harmonics 1..kMaxHarmonic-1.
struct Spectrum {
    std::vector<double> a = std::vector<double>(kMaxHarmonic, 0.0);
    std::vector<double> b = std::vector<double>(kMaxHarmonic, 0.0);
};

Spectrum mix(const Spectrum& x, const Spectrum& y, double t) {
    Spectrum s;
    for (int h = 1; h < kMaxHarmonic; ++h) {
        s.a[h] = x.a[h] + (y.a[h] - x.a[h]) * t;
        s.b[h] = x.b[h] + (y.b[h] - x.b[h]) * t;
    }
    return s;
}

// --- exact Fourier series of the classic shapes ---
Spectrum sine() {
    Spectrum s;
    s.b[1] = 1.0;
    return s;
}

Spectrum saw() {
    Spectrum s;
    for (int h = 1; h < kMaxHarmonic; ++h) s.b[h] = 2.0 / (kPi * h) * ((h & 1) ? 1.0 : -1.0);
    return s;
}

Spectrum square() {
    Spectrum s;
    for (int h = 1; h < kMaxHarmonic; h += 2) s.b[h] = 4.0 / (kPi * h);
    return s;
}

Spectrum triangle() {
    Spectrum s;
    for (int h = 1; h < kMaxHarmonic; h += 2) s.b[h] = 8.0 / (kPi * kPi * h * h) * (((h - 1) / 2) & 1 ? -1.0 : 1.0);
    return s;
}

// +1 for the first `duty` of the cycle, -1 after (DC removed).
Spectrum pulse(double duty) {
    Spectrum s;
    for (int h = 1; h < kMaxHarmonic; ++h) {
        s.a[h] = 2.0 * std::sin(2.0 * kPi * h * duty) / (kPi * h);
        s.b[h] = 2.0 * (1.0 - std::cos(2.0 * kPi * h * duty)) / (kPi * h);
    }
    return s;
}

// A soft 1/h body plus one resonant peak around harmonic `centre`: sweeping the centre
// across the frames gives the vowel-like "talking" movement.
Spectrum formant(double centre) {
    Spectrum s;
    const double width = 0.25 * centre + 1.5;
    for (int h = 1; h < kMaxHarmonic; ++h) {
        const double z = (h - centre) / width;
        s.b[h] = 0.3 / h + std::exp(-z * z);
    }
    return s;
}

// Spectrum of one cycle given as samples (x.size() a power of two; overwritten). Harmonics
// the cycle can't carry (h >= size/2) stay zero.
Spectrum fromSamples(std::vector<cd>& x) {
    const size_t m = x.size();
    fft(x, false);
    Spectrum s;
    const int top = static_cast<int>(std::min<size_t>(kMaxHarmonic, m / 2));
    for (int h = 1; h < top; ++h) {
        s.a[h] = 2.0 * x[static_cast<size_t>(h)].real() / static_cast<double>(m);
        s.b[h] = -2.0 * x[static_cast<size_t>(h)].imag() / static_cast<double>(m);
    }
    return s;
}

// Spectrum of a time-domain cycle f(t), t in [0,1), sampled 8x finer than a frame so the
// harmonics we keep carry almost none of the naive rendering's own aliasing (-78 dB).
Spectrum measure(const std::function<double(double)>& f) {
    const size_t m = static_cast<size_t>(kTableSize) * 8;
    std::vector<cd> x(m);
    for (size_t i = 0; i < m; ++i) x[i] = cd(f(static_cast<double>(i) / static_cast<double>(m)), 0.0);
    return fromSamples(x);
}

// Renders one frame at every mip level. `normalise`: scale by mip 0's peak, so all levels
// of a frame keep the same loudness and built-in frames don't jump in level as you morph.
void addFrame(Wavetable& t, const Spectrum& s, bool normalise = true) {
    const size_t base = t.data.size();
    t.data.resize(base + static_cast<size_t>(kMipLevels) * kFrameStride);
    std::vector<cd> x(kTableSize);
    double gain = 1.0;
    for (int k = 0; k < kMipLevels; ++k) {
        const int top = std::min(kMaxHarmonic - 1, kMaxHarmonic >> k);
        std::fill(x.begin(), x.end(), cd(0.0, 0.0));
        for (int h = 1; h <= top; ++h) {
            const cd c(s.a[h] * kTableSize / 2.0, -s.b[h] * kTableSize / 2.0);
            x[static_cast<size_t>(h)] = c;
            x[static_cast<size_t>(kTableSize - h)] = std::conj(c);
        }
        fft(x, true);
        if (k == 0 && normalise) {
            double peak = 0.0;
            for (const auto& v : x) peak = std::max(peak, std::abs(v.real()));
            gain = peak > 1e-9 ? 1.0 / peak : 1.0;
        }
        float* dst = &t.data[base + static_cast<size_t>(k) * kFrameStride];
        for (int i = 0; i < kTableSize; ++i) dst[i] = static_cast<float>(x[static_cast<size_t>(i)].real() * gain);
        dst[kTableSize] = dst[0];
    }
    ++t.frames;
}

constexpr int kFrames = 16;

std::vector<Wavetable> build() {
    std::vector<Wavetable> out(4);

    // Classic: sine -> triangle -> saw -> square, the key shapes on frames 0, 5, 10, 15.
    out[0].name = "Classic";
    const Spectrum keys[4] = {sine(), triangle(), saw(), square()};
    for (int f = 0; f < kFrames; ++f) {
        const double x = f / 5.0;
        const int i = std::min(static_cast<int>(x), 2);
        addFrame(out[0], mix(keys[i], keys[i + 1], x - i));
    }

    // PWM: duty cycle 50% -> 4%.
    out[1].name = "PWM";
    for (int f = 0; f < kFrames; ++f) addFrame(out[1], pulse(0.5 - 0.46 * f / (kFrames - 1)));

    // Sync: a saw hard-synced to the fundamental, slave ratio 1 -> 8 (exponential).
    out[2].name = "Sync";
    for (int f = 0; f < kFrames; ++f) {
        const double r = std::pow(8.0, static_cast<double>(f) / (kFrames - 1));
        addFrame(out[2], measure([r](double t) {
            const double p = t * r;
            return 2.0 * (p - std::floor(p)) - 1.0;
        }));
    }

    // Formant: a resonant peak sweeping harmonics 1 -> 48.
    out[3].name = "Formant";
    for (int f = 0; f < kFrames; ++f) addFrame(out[3], formant(std::pow(48.0, static_cast<double>(f) / (kFrames - 1))));

    return out;
}

} // namespace

const std::vector<Wavetable>& builtinTables() {
    static const std::vector<Wavetable> tables = build();   // thread-safe one-time init
    return tables;
}

// --- WAV import ---------------------------------------------------------------------------

namespace {

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

bool fail(std::string* err, const char* why) {
    if (err) *err = why;
    return false;
}

float readSample(const uint8_t* p, uint16_t tag, uint16_t bits) {
    if (tag == 3) {
        float v;
        std::memcpy(&v, p, sizeof v);
        return std::isfinite(v) ? v : 0.0f;
    }
    if (bits == 16) return static_cast<float>(static_cast<int16_t>(rd16(p))) / 32768.0f;
    if (bits == 24) {
        int32_t v = static_cast<int32_t>(p[0] | p[1] << 8 | p[2] << 16);
        if (v & 0x800000) v -= 0x1000000;
        return static_cast<float>(v) / 8388608.0f;
    }
    return static_cast<float>(static_cast<int32_t>(rd32(p))) / 2147483648.0f;
}

std::string stem(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    std::string s = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = s.rfind('.');
    return dot == std::string::npos ? s : s.substr(0, dot);
}

} // namespace

bool loadWavetable(const std::string& path, Wavetable& out, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail(err, "cannot open");
    const std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0)
        return fail(err, "not a WAV file");

    uint16_t tag = 0, channels = 0, bits = 0, align = 0;
    const uint8_t* data = nullptr;
    size_t dataSize = 0;
    int frameSize = kTableSize;
    for (size_t i = 12; i + 8 <= b.size();) {
        const uint8_t* c = &b[i];
        const size_t sz = rd32(c + 4);
        const size_t room = b.size() - (i + 8);
        const size_t avail = std::min(sz, room);   // a truncated last chunk: use what is there
        const uint8_t* body = c + 8;
        if (!std::memcmp(c, "fmt ", 4) && avail >= 16) {
            tag = rd16(body);
            channels = rd16(body + 2);
            align = rd16(body + 12);
            bits = rd16(body + 14);
            if (tag == 0xFFFE && avail >= 26) tag = rd16(body + 24);   // WAVE_FORMAT_EXTENSIBLE
        } else if (!std::memcmp(c, "data", 4)) {
            data = body;
            dataSize = avail;
        } else if (!std::memcmp(c, "clm ", 4) && avail > 3 && !std::memcmp(body, "<!>", 3)) {
            // Serum's marker: "<!>2048 ..." = samples per frame
            const std::string digits(reinterpret_cast<const char*>(body) + 3, std::min<size_t>(avail - 3, 8));
            frameSize = std::atoi(digits.c_str());
        }
        if (sz >= room) break;   // last chunk (no 32-bit size_t overflow on bogus sizes)
        i += 8 + sz + (sz & 1);
    }

    if (!data) return fail(err, "no data chunk");
    const bool pcm = tag == 1 && (bits == 16 || bits == 24 || bits == 32);
    if (!(tag == 3 && bits == 32) && !pcm) return fail(err, "unsupported sample format (float32 or 16/24/32-bit PCM)");
    if (channels < 1 || align < channels * (bits / 8)) return fail(err, "bad fmt chunk");
    if (frameSize < 64 || frameSize > 16384 || (frameSize & (frameSize - 1)) != 0)
        return fail(err, "frame size must be a power of two, 64..16384");
    const size_t frames = std::min<size_t>(dataSize / align / static_cast<size_t>(frameSize), kMaxFrames);
    if (frames == 0) return fail(err, "shorter than one frame");

    Wavetable t;
    t.name = stem(path);
    t.data.reserve(frames * kMipLevels * kFrameStride);
    std::vector<cd> x(static_cast<size_t>(frameSize));
    for (size_t fr = 0; fr < frames; ++fr) {
        for (size_t i = 0; i < x.size(); ++i)
            x[i] = cd(readSample(data + (fr * x.size() + i) * align, tag, bits), 0.0);
        addFrame(t, fromSamples(x), false);
    }

    float peak = 0.0f;
    for (int fr = 0; fr < t.frames; ++fr) {
        const float* m0 = t.get(fr, 0);
        for (int i = 0; i < kTableSize; ++i) peak = std::max(peak, std::fabs(m0[i]));
    }
    if (peak < 1e-6f) return fail(err, "silent");
    const float gain = 1.0f / peak;
    for (float& v : t.data) v *= gain;

    out = std::move(t);
    return true;
}

} // namespace pf
