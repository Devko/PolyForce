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

// In-place iterative radix-2 FFT for one size, load time only. Twiddles and the bit-reversal
// permutation are tabled once per plan (a recurrence drifts, and sin/cos per butterfly is
// slow on the Force). The inverse includes the 1/N. Butterflies multiply by hand:
// std::complex's operator* calls __muldc3 (NaN/inf fix-ups) unless -ffast-math.
class Fft {
public:
    explicit Fft(size_t n) : n_(n), w_(n / 2), rev_(n) {
        for (size_t k = 0; k < n / 2; ++k) {
            const double a = -2.0 * kPi * static_cast<double>(k) / static_cast<double>(n);
            w_[k] = cd(std::cos(a), std::sin(a));
        }
        for (size_t i = 1, j = 0; i < n; ++i) {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            rev_[i] = static_cast<uint32_t>(j);
        }
    }
    size_t size() const { return n_; }

    void run(std::vector<cd>& a, bool inverse) const {
        for (size_t i = 1; i < n_; ++i)
            if (i < rev_[i]) std::swap(a[i], a[rev_[i]]);
        for (size_t len = 2; len <= n_; len <<= 1) {
            const size_t step = n_ / len;
            for (size_t i = 0; i < n_; i += len) {
                for (size_t k = 0; k < len / 2; ++k) {
                    const cd w = w_[k * step];
                    const double wr = w.real(), wi = inverse ? -w.imag() : w.imag();
                    const cd u = a[i + k];
                    const cd x = a[i + k + len / 2];
                    const cd v(x.real() * wr - x.imag() * wi, x.real() * wi + x.imag() * wr);
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                }
            }
        }
        if (inverse)
            for (auto& x : a) x /= static_cast<double>(n_);
    }

private:
    size_t n_;
    std::vector<cd> w_;
    std::vector<uint32_t> rev_;
};

// The inverse plans of every mip length, shared by all builds in this call.
struct Plans {
    Fft p2048{2048}, p1024{1024}, p512{512}, p256{256};
    const Fft& forLength(int n) const {
        return n == 2048 ? p2048 : n == 1024 ? p1024 : n == 512 ? p512 : p256;
    }
};

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

// Spectra of two real cycles at once, x = first + i * second (x.size() a power of two;
// overwritten). Harmonics a cycle can't carry (h >= size/2) stay zero.
void fromSamples(std::vector<cd>& x, const Fft& fft, Spectrum& first, Spectrum& second) {
    const size_t m = x.size();
    fft.run(x, false);
    const int top = static_cast<int>(std::min<size_t>(kMaxHarmonic, m / 2));
    const double scale = 2.0 / static_cast<double>(m);
    for (int h = 1; h < top; ++h) {
        const cd xh = x[static_cast<size_t>(h)];
        const cd xm = std::conj(x[m - static_cast<size_t>(h)]);
        const cd A = 0.5 * (xh + xm);                 // spectrum of the real part
        const cd B = cd(0.0, -0.5) * (xh - xm);       // ... and of the imaginary part
        first.a[h] = scale * A.real();
        first.b[h] = -scale * A.imag();
        second.a[h] = scale * B.real();
        second.b[h] = -scale * B.imag();
    }
}

// Spectrum of a time-domain cycle f(t), t in [0,1), sampled 8x finer than a frame so the
// harmonics we keep carry almost none of the naive rendering's own aliasing (-78 dB).
Spectrum measure(const std::function<double(double)>& f) {
    const size_t m = static_cast<size_t>(kTableSize) * 8;
    const Fft fft(m);
    std::vector<cd> x(m);
    for (size_t i = 0; i < m; ++i) x[i] = cd(f(static_cast<double>(i) / static_cast<double>(m)), 0.0);
    Spectrum s, unused;
    fromSamples(x, fft, s, unused);
    return s;
}

// Renders up to two frames (b may be null) at every mip level, appended to t, one inverse
// FFT per level for both. `normalise`: scale each frame by its level-0 peak, so built-in
// frames don't jump in level as you morph.
void addFrames(Wavetable& t, const Plans& plans, const Spectrum& sa, const Spectrum* sb, bool normalise) {
    const int count = sb ? 2 : 1;
    const size_t base = t.data.size();
    t.data.resize(base + static_cast<size_t>(count) * kFrameStride);
    std::vector<cd> x(static_cast<size_t>(kTableSize));
    double gain[2] = {1.0, 1.0};
    for (int k = 0; k < kMipLevels; ++k) {
        const int n = mipLength(k);
        const int top = std::min(kMaxHarmonic - 1, kMaxHarmonic >> k);
        x.assign(static_cast<size_t>(n), cd(0.0, 0.0));
        const double half = n / 2.0;
        for (int h = 1; h <= top; ++h) {
            // X = Ca + i * Cb, where Ca/Cb are the Hermitian spectra of the two real frames.
            const cd ca(sa.a[h] * half, -sa.b[h] * half);
            const cd cb = sb ? cd(sb->a[h] * half, -sb->b[h] * half) : cd(0.0, 0.0);
            const cd i(0.0, 1.0);
            x[static_cast<size_t>(h)] = ca + i * cb;
            x[static_cast<size_t>(n - h)] = std::conj(ca) + i * std::conj(cb);
        }
        plans.forLength(n).run(x, true);
        if (k == 0 && normalise) {
            double peak[2] = {0.0, 0.0};
            for (const auto& v : x) {
                peak[0] = std::max(peak[0], std::abs(v.real()));
                peak[1] = std::max(peak[1], std::abs(v.imag()));
            }
            for (int f = 0; f < 2; ++f) gain[f] = peak[f] > 1e-9 ? 1.0 / peak[f] : 1.0;
        }
        for (int f = 0; f < count; ++f) {
            float* dst = &t.data[base + static_cast<size_t>(f) * kFrameStride + static_cast<size_t>(mipOffset(k))];
            for (int s = 0; s < n; ++s) {
                const cd v = x[static_cast<size_t>(s)];
                dst[s] = static_cast<float>((f ? v.imag() : v.real()) * gain[f]);
            }
            dst[n] = dst[0];
        }
    }
    t.frames += count;
}

void addSpectra(Wavetable& t, const Plans& plans, const std::vector<Spectrum>& specs, bool normalise) {
    for (size_t i = 0; i < specs.size(); i += 2)
        addFrames(t, plans, specs[i], i + 1 < specs.size() ? &specs[i + 1] : nullptr, normalise);
}

constexpr int kFrames = 16;
constexpr int kPulseFrames = 32;

struct Tables {
    std::vector<Wavetable> builtin;
    Wavetable classic[CW_COUNT];
};

Tables build() {
    const Plans plans;
    Tables out;
    auto& b = out.builtin;
    b.resize(4);

    // Classic: sine -> triangle -> saw -> square, the key shapes on frames 0, 5, 10, 15.
    b[0].name = "Classic";
    const Spectrum keys[4] = {sine(), triangle(), saw(), square()};
    std::vector<Spectrum> specs;
    for (int f = 0; f < kFrames; ++f) {
        const double x = f / 5.0;
        const int i = std::min(static_cast<int>(x), 2);
        specs.push_back(mix(keys[i], keys[i + 1], x - i));
    }
    addSpectra(b[0], plans, specs, true);

    // PWM: duty cycle 50% -> 4%.
    b[1].name = "PWM";
    specs.clear();
    for (int f = 0; f < kFrames; ++f) specs.push_back(pulse(0.5 - 0.46 * f / (kFrames - 1)));
    addSpectra(b[1], plans, specs, true);

    // Sync: a saw hard-synced to the fundamental, slave ratio 1 -> 8 (exponential).
    b[2].name = "Sync";
    specs.clear();
    for (int f = 0; f < kFrames; ++f) {
        const double r = std::pow(8.0, static_cast<double>(f) / (kFrames - 1));
        specs.push_back(measure([r](double t) {
            const double p = t * r;
            return 2.0 * (p - std::floor(p)) - 1.0;
        }));
    }
    addSpectra(b[2], plans, specs, true);

    // Formant: a resonant peak sweeping harmonics 1 -> 48.
    b[3].name = "Formant";
    specs.clear();
    for (int f = 0; f < kFrames; ++f) specs.push_back(formant(std::pow(48.0, static_cast<double>(f) / (kFrames - 1))));
    addSpectra(b[3], plans, specs, true);

    // The classic oscillator shapes. Not normalised per frame: a square is louder than a
    // sine, as on an analog synth. Saw and square peak above 1 by their Gibbs overshoot only.
    const char* names[CW_COUNT] = {"Sine", "Triangle", "Saw", "Square", "Pulse"};
    const Spectrum shapes[4] = {sine(), triangle(), saw(), square()};
    for (int w = 0; w < CW_PULSE; ++w) {
        out.classic[w].name = names[w];
        addFrames(out.classic[w], plans, shapes[w], nullptr, false);
    }
    out.classic[CW_PULSE].name = names[CW_PULSE];
    specs.clear();
    for (int f = 0; f < kPulseFrames; ++f) specs.push_back(pulse(0.5 - 0.47 * f / (kPulseFrames - 1)));
    addSpectra(out.classic[CW_PULSE], plans, specs, false);
    return out;
}

const Tables& tables() {
    static const Tables t = build();   // thread-safe one-time init
    return t;
}

} // namespace

const std::vector<Wavetable>& builtinTables() { return tables().builtin; }

const Wavetable& classicTable(int wave) { return tables().classic[std::clamp(wave, 0, CW_COUNT - 1)]; }

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

    const Plans plans;
    const Fft forward(static_cast<size_t>(frameSize));
    Wavetable t;
    t.name = stem(path);
    t.data.reserve(frames * kFrameStride);
    std::vector<cd> x(static_cast<size_t>(frameSize));
    Spectrum sa, sb;
    for (size_t fr = 0; fr < frames; fr += 2) {
        const bool pair = fr + 1 < frames;
        for (size_t i = 0; i < x.size(); ++i) {
            const float re = readSample(data + (fr * x.size() + i) * align, tag, bits);
            const float im = pair ? readSample(data + ((fr + 1) * x.size() + i) * align, tag, bits) : 0.0f;
            x[i] = cd(re, im);
        }
        fromSamples(x, forward, sa, sb);
        addFrames(t, plans, sa, pair ? &sb : nullptr, false);
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
