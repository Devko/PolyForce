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

// Where frame f of n sits along a table, 0..1.
double at(int f, int n) { return n > 1 ? static_cast<double>(f) / static_cast<double>(n - 1) : 0.0; }

// Spectra of time-domain cycles, sampled 8x finer than a frame so the harmonics we keep carry
// almost none of the naive rendering's own aliasing (-78 dB). One plan per table, two cycles
// per FFT.
class Sampler {
public:
    static constexpr size_t kLen = static_cast<size_t>(kTableSize) * 8;
    Sampler() : fft_(kLen), x_(kLen) {}

    // n frames: make(x) for x = 0..1 across the frames returns that frame's cycle g(t), t in [0,1).
    template <class Make> std::vector<Spectrum> frames(int n, Make make) {
        std::vector<Spectrum> out(static_cast<size_t>(n));
        Spectrum unused;
        for (int i = 0; i < n; i += 2) {
            const bool pair = i + 1 < n;
            const auto ga = make(at(i, n));
            const auto gb = make(pair ? at(i + 1, n) : 0.0);
            for (size_t s = 0; s < kLen; ++s) {
                const double t = static_cast<double>(s) / static_cast<double>(kLen);
                x_[s] = cd(ga(t), pair ? gb(t) : 0.0);
            }
            fromSamples(x_, fft_, out[static_cast<size_t>(i)], pair ? out[static_cast<size_t>(i) + 1] : unused);
        }
        return out;
    }

    // Every frame through a waveshaper: rendered at peak 1, then shape(frame, sample) per sample,
    // then measured again.
    template <class Shape> void shape(std::vector<Spectrum>& specs, Shape shape) {
        const double half = static_cast<double>(kLen) / 2.0;
        const cd i1(0.0, 1.0);
        for (size_t f = 0; f < specs.size(); f += 2) {
            const bool pair = f + 1 < specs.size();
            const Spectrum& a = specs[f];
            const Spectrum* b = pair ? &specs[f + 1] : nullptr;
            x_.assign(kLen, cd(0.0, 0.0));
            for (int h = 1; h < kMaxHarmonic; ++h) {   // x = frame a + i * frame b, as in addFrames
                const cd ca(a.a[h] * half, -a.b[h] * half);
                const cd cb = b ? cd(b->a[h] * half, -b->b[h] * half) : cd(0.0, 0.0);
                x_[static_cast<size_t>(h)] = ca + i1 * cb;
                x_[kLen - static_cast<size_t>(h)] = std::conj(ca) + i1 * std::conj(cb);
            }
            fft_.run(x_, true);
            double pa = 0.0, pb = 0.0;
            for (const cd& v : x_) {
                pa = std::max(pa, std::fabs(v.real()));
                pb = std::max(pb, std::fabs(v.imag()));
            }
            const double ka = pa > 1e-12 ? 1.0 / pa : 1.0, kb = pb > 1e-12 ? 1.0 / pb : 1.0;
            const int fa = static_cast<int>(f), fb = fa + 1;
            for (cd& v : x_) v = cd(shape(fa, v.real() * ka), pair ? shape(fb, v.imag() * kb) : 0.0);
            Spectrum unused;
            fromSamples(x_, fft_, specs[f], pair ? specs[f + 1] : unused);
        }
    }

private:
    Fft fft_;
    std::vector<cd> x_;
};

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
    t.data.reserve(t.data.size() + specs.size() * kFrameStride);
    for (size_t i = 0; i < specs.size(); i += 2)
        addFrames(t, plans, specs[i], i + 1 < specs.size() ? &specs[i + 1] : nullptr, normalise);
}

// The highest |sample| of any frame's full-band level.
float peakOf(const Wavetable& t) {
    float peak = 0.0f;
    for (int fr = 0; fr < t.frames; ++fr) {
        const float* m0 = t.get(fr, 0);
        for (int i = 0; i < kTableSize; ++i) peak = std::max(peak, std::fabs(m0[i]));
    }
    return peak;
}

// One gain for the whole table (peak 1): the level differences between frames survive.
void normaliseWhole(Wavetable& t) {
    const float peak = peakOf(t);
    if (peak < 1e-9f) return;
    const float gain = 1.0f / peak;
    for (float& v : t.data) v *= gain;
}

// --- building blocks for the built-in tables -------------------------------------------------

// n frames morphing through the keys, evenly spaced: key k sits on frame k * (n - 1) / (keys - 1).
std::vector<Spectrum> morph(const std::vector<Spectrum>& keys, int n) {
    const int last = static_cast<int>(keys.size()) - 1;
    std::vector<Spectrum> out;
    for (int f = 0; f < n; ++f) {
        if (last < 1) {
            out.push_back(keys.front());
            continue;
        }
        const double x = static_cast<double>(f) * last / static_cast<double>(n - 1);
        const int i = std::min(static_cast<int>(x), last - 1);
        out.push_back(mix(keys[static_cast<size_t>(i)], keys[static_cast<size_t>(i) + 1], x - i));
    }
    return out;
}

// Harmonic amplitudes with a saw's phases (alternating sign): the sharp-edged shape of a
// bowed, blown or buzzing source.
template <class F> Spectrum sawPhased(F amp) {
    Spectrum s;
    for (int h = 1; h < kMaxHarmonic; ++h) s.b[h] = amp(h) * ((h & 1) ? 1.0 : -1.0);
    return s;
}

// A few partials, each at its own fixed phase (golden-ratio spread, so their peaks never all
// line up), weighted by w(h).
struct Partial {
    int h;
    double amp;
};
template <class W> Spectrum sparse(const std::vector<Partial>& ps, W w) {
    Spectrum s;
    for (size_t i = 0; i < ps.size(); ++i) {
        const int h = ps[i].h;
        if (h < 1 || h >= kMaxHarmonic) continue;
        const double ph = 2.0 * kPi * std::fmod(0.6180339887 * static_cast<double>(i), 1.0);
        const double a = ps[i].amp * w(h);
        s.a[h] += a * std::sin(ph);
        s.b[h] += a * std::cos(ph);
    }
    return s;
}
Spectrum sparse(const std::vector<Partial>& ps) {
    return sparse(ps, [](int) { return 1.0; });
}

// A filter applied to a spectrum: each harmonic times the (complex) response at it.
template <class H> Spectrum filtered(const Spectrum& s, H response) {
    Spectrum o;
    for (int h = 1; h < kMaxHarmonic; ++h) {
        const cd c = cd(s.a[h], -s.b[h]) * response(h);   // a cos + b sin = Re((a - ib) e^(i wt))
        o.a[h] = c.real();
        o.b[h] = -c.imag();
    }
    return o;
}

// Second-order low-pass at harmonic h: cutoff c (in harmonics), quality q.
cd lowpass2(double h, double c, double q) {
    const double x = h / c;
    return 1.0 / cd(1.0 - x * x, x / q);
}

// A resonance's magnitude at f: 1 at its centre, 1/sqrt(2) half a bandwidth away.
double peak(double f, double centre, double bw) {
    const double z = (f - centre) / (0.5 * bw);
    return 1.0 / std::sqrt(1.0 + z * z);
}

// Sung vowels: a glottal source (spectral tilt `tilt`) through five formants (Hz, dB, bandwidth),
// computed for one fundamental: the formants sit on fixed harmonics, so the vowel is truest
// near that pitch. Frames morph the formants themselves (frequency, level, width), so the
// resonances glide from vowel to vowel instead of crossfading.
struct Formant {
    double hz, db, bw;
};
struct Vowel {
    Formant f[5];
};

// Formant sets after the Csound manual's vowel table.
constexpr Vowel kBassA{{{600, 0, 60}, {1040, -7, 70}, {2250, -9, 110}, {2450, -9, 120}, {2750, -20, 130}}};
constexpr Vowel kBassE{{{400, 0, 40}, {1620, -12, 80}, {2400, -9, 100}, {2800, -12, 120}, {3100, -18, 120}}};
constexpr Vowel kBassI{{{250, 0, 60}, {1750, -30, 90}, {2600, -16, 100}, {3050, -22, 120}, {3340, -28, 120}}};
constexpr Vowel kBassO{{{400, 0, 40}, {750, -11, 80}, {2400, -21, 100}, {2600, -20, 120}, {2900, -40, 120}}};
constexpr Vowel kBassU{{{350, 0, 40}, {600, -20, 80}, {2400, -32, 100}, {2675, -28, 120}, {2950, -36, 120}}};
constexpr Vowel kTenorA{{{650, 0, 80}, {1080, -6, 90}, {2650, -7, 120}, {2900, -8, 130}, {3250, -22, 140}}};
constexpr Vowel kTenorE{{{400, 0, 70}, {1700, -14, 80}, {2600, -12, 100}, {3200, -14, 120}, {3580, -20, 120}}};
constexpr Vowel kTenorI{{{290, 0, 40}, {1870, -15, 90}, {2800, -18, 100}, {3250, -20, 120}, {3540, -30, 120}}};
constexpr Vowel kTenorO{{{400, 0, 40}, {800, -10, 80}, {2600, -12, 100}, {2800, -12, 120}, {3000, -26, 120}}};
constexpr Vowel kTenorU{{{350, 0, 40}, {600, -20, 60}, {2700, -17, 100}, {2900, -14, 120}, {3300, -26, 120}}};
constexpr Vowel kAltoA{{{800, 0, 80}, {1150, -4, 90}, {2800, -20, 120}, {3500, -36, 130}, {4950, -60, 140}}};
constexpr Vowel kAltoE{{{400, 0, 60}, {1600, -24, 80}, {2700, -30, 120}, {3300, -35, 150}, {4950, -60, 200}}};
constexpr Vowel kAltoO{{{450, 0, 70}, {800, -9, 80}, {2830, -16, 100}, {3500, -28, 130}, {4950, -55, 135}}};
constexpr Vowel kAltoU{{{325, 0, 50}, {700, -12, 60}, {2530, -30, 170}, {3500, -40, 180}, {4950, -64, 200}}};

Vowel between(const Vowel& a, const Vowel& b, double t) {
    Vowel v{};
    for (int k = 0; k < 5; ++k) {
        v.f[k].hz = a.f[k].hz * std::pow(b.f[k].hz / a.f[k].hz, t);
        v.f[k].db = a.f[k].db + (b.f[k].db - a.f[k].db) * t;
        v.f[k].bw = a.f[k].bw + (b.f[k].bw - a.f[k].bw) * t;
    }
    return v;
}

struct Voice {
    double f0, tilt, floor, widen;   // fundamental (Hz), source tilt, unformanted floor, bandwidth scale
};

Spectrum sing(const Vowel& v, const Voice& c) {
    return sawPhased([&](int h) {
        const double f = h * c.f0;
        if (f > 12000.0) return 0.0;
        double g = c.floor;
        for (const Formant& m : v.f) g += std::pow(10.0, m.db / 20.0) * peak(f, m.hz, std::max(100.0, m.bw * c.widen));
        return g / std::pow(static_cast<double>(h), c.tilt);
    });
}

std::vector<Spectrum> vowels(const std::vector<Vowel>& keys, int n, const Voice& c) {
    const int last = static_cast<int>(keys.size()) - 1;
    std::vector<Spectrum> out;
    for (int f = 0; f < n; ++f) {
        const double x = static_cast<double>(f) * last / static_cast<double>(n - 1);
        const int i = std::min(static_cast<int>(x), last - 1);
        out.push_back(sing(between(keys[static_cast<size_t>(i)], keys[static_cast<size_t>(i) + 1], x - i), c));
    }
    return out;
}

// --- the built-in tables -----------------------------------------------------------------------

constexpr int kFrames = 16;        // the first four tables, and the short ones
constexpr int kLongFrames = 32;    // most of the others
constexpr int kPulseFrames = 32;

// Classic: sine -> triangle -> saw -> square, the key shapes on frames 0, 5, 10, 15.
void makeClassic(Wavetable& t, const Plans& p) {
    addSpectra(t, p, morph({sine(), triangle(), saw(), square()}, kFrames), true);
}

// PWM: duty cycle 50% -> 4%.
void makePwm(Wavetable& t, const Plans& p) {
    std::vector<Spectrum> specs;
    for (int f = 0; f < kFrames; ++f) specs.push_back(pulse(0.5 - 0.46 * f / (kFrames - 1)));
    addSpectra(t, p, specs, true);
}

// Sync: a saw hard-synced to the fundamental, slave ratio 1 -> 8 (exponential).
void makeSync(Wavetable& t, const Plans& p) {
    Sampler s;
    addSpectra(t, p, s.frames(kFrames, [](double x) {
        const double r = std::pow(8.0, x);
        return [r](double tt) { return 2.0 * (tt * r - std::floor(tt * r)) - 1.0; };
    }), true);
}

// Formant: a resonant peak sweeping harmonics 1 -> 48.
void makeFormant(Wavetable& t, const Plans& p) {
    std::vector<Spectrum> specs;
    for (int f = 0; f < kFrames; ++f) specs.push_back(formant(std::pow(48.0, at(f, kFrames))));
    addSpectra(t, p, specs, true);
}

// Square Sync: a square hard-synced to the fundamental, slave ratio 1 -> 8.
void makeSquareSync(Wavetable& t, const Plans& p) {
    Sampler s;
    addSpectra(t, p, s.frames(kLongFrames, [](double x) {
        const double r = std::pow(8.0, x);
        return [r](double tt) { return tt * r - std::floor(tt * r) < 0.5 ? 1.0 : -1.0; };
    }), true);
}

// Reso Saw / Reso Square: the shape through a resonant 24 dB low-pass whose cutoff sweeps
// harmonic 1.5 -> 96: a filter sweep baked into the position (acid, talking basses).
void makeReso(Wavetable& t, const Plans& p, const Spectrum& src) {
    std::vector<Spectrum> specs;
    for (int f = 0; f < kLongFrames; ++f) {
        const double c = 1.5 * std::pow(64.0, at(f, kLongFrames));
        specs.push_back(filtered(src, [c](int h) { return lowpass2(h, c, 0.707) * lowpass2(h, c, 4.0); }));
    }
    addSpectra(t, p, specs, true);
}
void makeResoSaw(Wavetable& t, const Plans& p) { makeReso(t, p, saw()); }
void makeResoSquare(Wavetable& t, const Plans& p) { makeReso(t, p, square()); }

// Harmonics: additive, one more harmonic per frame (1 -> 32), each 1/h^0.7: a buzzy, organ-
// like brightness control with no filter.
void makeHarmonics(Wavetable& t, const Plans& p) {
    std::vector<Spectrum> specs;
    for (int f = 0; f < kLongFrames; ++f)
        specs.push_back(sawPhased([f](int h) { return h <= f + 1 ? std::pow(static_cast<double>(h), -0.7) : 0.0; }));
    addSpectra(t, p, specs, true);
}

// Comb Saw: a saw plus a delayed copy of itself, the delay growing 1/512 -> 1/4 of a cycle: comb
// notches sweep down through the spectrum like a flanger.
void makeCombSaw(Wavetable& t, const Plans& p) {
    std::vector<Spectrum> specs;
    const Spectrum src = saw();
    for (int f = 0; f < kLongFrames; ++f) {
        const double tau = std::pow(128.0, at(f, kLongFrames)) / 512.0;
        specs.push_back(filtered(src, [tau](int h) { return 0.5 * (1.0 + std::polar(1.0, -2.0 * kPi * h * tau)); }));
    }
    addSpectra(t, p, specs, true);
}

// Fold: a sine through a triangle wavefolder, gain 1 -> 8, a touch of bias for even harmonics.
void makeFold(Wavetable& t, const Plans& p) {
    Sampler s;
    addSpectra(t, p, s.frames(kLongFrames, [](double x) {
        const double gain = 1.0 + 7.0 * std::pow(x, 1.5), bias = 0.15 * x;
        return [gain, bias](double tt) {
            const double v = gain * (std::sin(2.0 * kPi * tt) + bias);
            return std::asin(std::sin(0.5 * kPi * v)) * (2.0 / kPi);
        };
    }), true);
}

// Phase Dist: a cosine read through a bent phase (Casio CZ): the knee moves from the middle of
// the cycle to its start, sine -> a sharp saw.
void makePhaseDist(Wavetable& t, const Plans& p) {
    Sampler s;
    addSpectra(t, p, s.frames(kLongFrames, [](double x) {
        const double d = 0.5 - 0.48 * x;
        return [d](double tt) {
            const double ph = tt < d ? 0.5 * tt / d : 0.5 + 0.5 * (tt - d) / (1.0 - d);
            return -std::cos(2.0 * kPi * ph);
        };
    }), true);
}

// CZ Reso: a sine at 1 -> 16 times the fundamental under a falling window (Casio CZ resonance):
// a resonant peak sweeping up without a filter.
void makeCzReso(Wavetable& t, const Plans& p) {
    Sampler s;
    addSpectra(t, p, s.frames(kLongFrames, [](double x) {
        const double r = std::pow(16.0, x);
        return [r](double tt) { return (1.0 - tt) * std::sin(2.0 * kPi * r * tt); };
    }), true);
}

// FM: a sine phase-modulated by a sine at `ratio` times the fundamental, index 0 -> `index`.
void makeFm(Wavetable& t, const Plans& p, int ratio, double index) {
    Sampler s;
    addSpectra(t, p, s.frames(kLongFrames, [ratio, index](double x) {
        const double i = index * x;
        return [ratio, i](double tt) { return std::sin(2.0 * kPi * tt + i * std::sin(2.0 * kPi * ratio * tt)); };
    }), true);
}
void makeFm1(Wavetable& t, const Plans& p) { makeFm(t, p, 1, 6.0); }
void makeFm2(Wavetable& t, const Plans& p) { makeFm(t, p, 2, 5.0); }
void makeFm3(Wavetable& t, const Plans& p) { makeFm(t, p, 3, 4.0); }

// FM Tine: a sine with a 1:1 and a 14:1 modulator, both rising: the bell-like "tine" of an FM
// electric piano. Frame 0 is a pure sine, so an envelope to position decays the tine away.
void makeFmTine(Wavetable& t, const Plans& p) {
    Sampler s;
    addSpectra(t, p, s.frames(kLongFrames, [](double x) {
        return [x](double tt) {
            const double w = 2.0 * kPi * tt;
            return std::sin(w + 0.6 * x * std::sin(w) + 2.2 * x * std::sin(14.0 * w));
        };
    }), true);
}

// Digital: eight seeded random spectra (a bright band somewhere over a 1/h body, random signs),
// morphed in turn: the stepping, glassy timbres of early digital wavetables.
void makeDigital(Wavetable& t, const Plans& p) {
    uint32_t r = 0x9e3779b9u;
    auto rnd = [&r] {
        r ^= r << 13;
        r ^= r >> 17;
        r ^= r << 5;
        return static_cast<double>(r >> 8) * (1.0 / 16777216.0);
    };
    std::vector<Spectrum> keys;
    for (int k = 0; k < 8; ++k) {
        const double centre = std::pow(2.0, 1.0 + 5.0 * rnd());   // harmonic 2 .. 64
        const double width = 0.3 + 0.7 * rnd();                   // octaves
        Spectrum s;
        for (int h = 1; h <= 96; ++h) {
            const double z = std::log2(h / centre) / width;
            const double a = (0.3 + 0.7 * rnd()) * (0.25 + 1.5 * std::exp(-z * z)) / h;
            s.b[h] = rnd() < 0.5 ? a : -a;
        }
        keys.push_back(s);
    }
    addSpectra(t, p, morph(keys, kLongFrames), true);
}

// Bitcrush: a sine with ever fewer levels (64 -> 2) and steps per cycle (256 -> 16).
void makeBitcrush(Wavetable& t, const Plans& p) {
    Sampler s;
    addSpectra(t, p, s.frames(kLongFrames, [](double x) {
        const double half = 0.5 * std::pow(2.0, 6.0 - 5.0 * x);
        const double steps = std::round(256.0 * std::pow(1.0 / 16.0, x));
        return [half, steps](double tt) {
            const double v = std::sin(2.0 * kPi * std::floor(tt * steps) / steps);
            return std::round(v * half) / half;
        };
    }), true);
}

// Chip: the shapes of 8-bit sound chips, one per frame: pulse 12.5% / 25% / 50%, the NES
// triangle (4 bits, 32 steps), a 4-bit saw and sine, an octave-pulse pair and a 4-bit wave of
// the kind Game Boy tunes load. Band-limited like everything else (no aliasing), so position
// 0, 1/7, 2/7 ... picks a shape.
void makeChip(Wavetable& t, const Plans& p) {
    constexpr int kWave[32] = {8, 11, 13, 14, 15, 15, 14, 13, 11, 8, 6, 4, 3, 2, 2, 3,
                               15, 15, 15, 15, 0, 0, 0, 0, 4, 6, 8, 10, 12, 10, 8, 6};
    auto stair = [](double v, int levels) {   // -1..1 onto `levels` steps
        return std::round((v + 1.0) * 0.5 * (levels - 1)) / (levels - 1) * 2.0 - 1.0;
    };
    Sampler s;
    addSpectra(t, p, s.frames(8, [&](double x) {
        return [&stair, &kWave, shape = static_cast<int>(std::lround(x * 7.0))](double tt) {
        switch (shape) {
            case 0: return tt < 0.125 ? 1.0 : -1.0;
            case 1: return tt < 0.25 ? 1.0 : -1.0;
            case 2: return tt < 0.5 ? 1.0 : -1.0;
            case 3: {
                const double q = std::floor(tt * 32.0) / 32.0;
                return stair(q < 0.5 ? 4.0 * q - 1.0 : 3.0 - 4.0 * q, 16);
            }
            case 4: return stair(2.0 * std::floor(tt * 16.0) / 16.0 - 1.0, 16);
            case 5: return stair(std::sin(2.0 * kPi * std::floor(tt * 32.0) / 32.0), 16);
            case 6: return 0.5 * ((tt < 0.5 ? 1.0 : -1.0) + (std::fmod(2.0 * tt, 1.0) < 0.25 ? 1.0 : -1.0));
            default: return kWave[std::min(static_cast<int>(tt * 32.0), 31)] / 7.5 - 1.0;
        }
        };
    }), true);
}

// Vowels: a tenor singing A -> E -> I -> O -> U (formants for C3).
void makeVowels(Wavetable& t, const Plans& p) {
    addSpectra(t, p, vowels({kTenorA, kTenorE, kTenorI, kTenorO, kTenorU}, kLongFrames, {130.8, 1.0, 0.03, 1.6}), true);
}

// Choir: a soft alto "oo -> oh -> ah -> eh" (formants for A3, wide and smooth for pads).
void makeChoir(Wavetable& t, const Plans& p) {
    addSpectra(t, p, vowels({kAltoU, kAltoO, kAltoA, kAltoE}, kLongFrames, {220.0, 1.25, 0.02, 1.8}), true);
}

// Growl: a bass voice U -> O -> A -> E -> I on a bright source (formants for C2), saturated:
// the "wow" of growl and wobble basses as the position moves.
void makeGrowl(Wavetable& t, const Plans& p) {
    Sampler s;
    std::vector<Spectrum> specs = vowels({kBassU, kBassO, kBassA, kBassE, kBassI}, kLongFrames, {65.4, 0.55, 0.12, 1.3});
    s.shape(specs, [](int, double v) { return std::tanh(2.2 * v); });
    addSpectra(t, p, specs, true);
}

// Organ: tonewheel drawbar registrations, soft -> full. Harmonic 1 is the 16' drawbar, so the
// 8' (the played pitch on an organ) is harmonic 2: play it an octave down.
void makeOrgan(Wavetable& t, const Plans& p) {
    static const char* const kRegs[kFrames] = {"008000000", "008500000", "008800000", "608800000",
                                               "808800000", "838800000", "888800000", "888804000",
                                               "888806000", "888808000", "888808400", "888808640",
                                               "888808860", "888888880", "888888884", "888888888"};
    constexpr int kFoot[9] = {1, 3, 2, 4, 6, 8, 10, 12, 16};   // 16' 5 1/3' 8' 4' 2 2/3' 2' 1 3/5' 1 1/3' 1'
    std::vector<Spectrum> specs;
    for (const char* reg : kRegs) {
        std::vector<Partial> ps;
        for (int d = 0; d < 9; ++d) {
            const int n = reg[d] - '0';   // drawbar 0..8, 3 dB a step
            ps.push_back({kFoot[d], n ? std::pow(10.0, -3.0 * (8 - n) / 20.0) : 0.0});
        }
        specs.push_back(sparse(ps));
    }
    addSpectra(t, p, specs, true);
}

// E-Piano: a soft 1:1 FM tone into an asymmetric saturator, both growing: mellow -> the bark of
// a hard-struck electric piano (map velocity or an envelope to position).
void makeEPiano(Wavetable& t, const Plans& p) {
    Sampler s;
    addSpectra(t, p, s.frames(kFrames, [](double x) {
        const double index = 0.15 + 1.6 * x, d = 1.0 + 2.5 * x, b = 0.3;
        return [index, d, b](double tt) {
            const double w = 2.0 * kPi * tt;
            const double v = std::sin(w + index * std::sin(w));
            return std::tanh(d * (v + b)) - std::tanh(d * b);
        };
    }), true);
}

// Strings: a bowed string (saw source) through a violin-like body (air and wood resonances,
// the bridge hill), dark (sul tasto) -> bright (sul ponticello). Body for A3.
void makeStrings(Wavetable& t, const Plans& p) {
    std::vector<Spectrum> specs;
    for (int f = 0; f < kFrames; ++f) {
        const double x = at(f, kFrames);
        specs.push_back(sawPhased([x](int h) {
            const double hz = h * 220.0;
            const double body = 0.35 + 1.0 * peak(hz, 290, 90) + 0.8 * peak(hz, 470, 160) + 1.6 * peak(hz, 1000, 500) +
                                1.3 * peak(hz, 2600, 900) + 0.6 * peak(hz, 4200, 1500);
            return body / std::pow(static_cast<double>(h), 1.7 - 0.9 * x) * std::exp(-hz / (3000.0 + 6000.0 * x));
        }));
    }
    addSpectra(t, p, specs, true);
}

// Brass: soft -> blaring, the brightness rising with the dynamic as it does in a horn (the
// strongest harmonic climbs from the 1st to about the 8th), a formant near 1.2 kHz, then a
// little saturation. Spectra for Bb3.
void makeBrass(Wavetable& t, const Plans& p) {
    std::vector<Spectrum> specs;
    for (int f = 0; f < kFrames; ++f) {
        const double fc = 350.0 * std::pow(9.0, at(f, kFrames));
        specs.push_back(sawPhased([fc](int h) {
            const double hz = h * 233.0;
            return std::pow(static_cast<double>(h), 0.6) * std::exp(-hz / fc) * (1.0 + 0.8 * peak(hz, 1200, 800));
        }));
    }
    Sampler s;
    s.shape(specs, [](int f, double v) {
        const double d = 0.5 + 1.5 * at(f, kFrames);
        return std::tanh(d * v) / std::tanh(d);
    });
    addSpectra(t, p, specs, true);
}

// Reed: clarinet (odd harmonics) -> oboe (a nasal formant) -> saxophone (bright). For C4.
void makeReed(Wavetable& t, const Plans& p) {
    const Spectrum clarinet = sawPhased([](int h) {
        const double hz = h * 262.0;
        return ((h & 1) ? 1.0 : (h < 12 ? 0.05 : 0.5)) / h * std::exp(-hz / 5000.0);
    });
    const Spectrum oboe = sawPhased([](int h) {
        const double hz = h * 262.0;
        return (1.0 + 3.0 * peak(hz, 1100, 600) + 1.5 * peak(hz, 2900, 900)) / std::pow(static_cast<double>(h), 1.1);
    });
    const Spectrum sax = sawPhased([](int h) {
        const double hz = h * 262.0;
        return (1.0 + 2.0 * peak(hz, 550, 300) + 1.5 * peak(hz, 1500, 700)) / std::pow(static_cast<double>(h), 0.75) *
               std::exp(-hz / 7000.0);
    });
    addSpectra(t, p, morph({clarinet, oboe, sax}, kFrames), true);
}

// Pluck: a string plucked near its end, bright -> dull as its high harmonics die away. One gain
// for the whole table, so the dull frames are as much quieter as a ringing string gets: set
// position 1 and Env 2 > Pos -1 for a pluck that darkens as it rings.
void makePluck(Wavetable& t, const Plans& p) {
    std::vector<Spectrum> specs;
    for (int f = 0; f < kLongFrames; ++f) {
        const double k = 0.004 * std::pow(150.0, at(f, kLongFrames));
        Spectrum s;
        for (int h = 1; h < kMaxHarmonic; ++h)
            s.b[h] = std::sin(kPi * h * 0.15) / std::pow(static_cast<double>(h), 1.3) * std::exp(-h * k);
        specs.push_back(s);
    }
    addSpectra(t, p, specs, false);
    normaliseWhole(t);
}

// Mallet: struck bars, soft mallet -> marimba -> vibraphone -> xylophone -> glockenspiel ->
// kalimba (key frames at position 0, 0.2 ... 1). Their partials rounded to harmonics.
void makeMallet(Wavetable& t, const Plans& p) {
    addSpectra(t, p, morph({sparse({{1, 1.0}, {4, 0.12}}),
                            sparse({{1, 1.0}, {4, 0.35}, {10, 0.12}}),
                            sparse({{1, 1.0}, {4, 0.55}, {10, 0.3}}),
                            sparse({{1, 1.0}, {3, 0.55}, {6, 0.35}, {10, 0.2}}),
                            sparse({{1, 1.0}, {3, 0.35}, {5, 0.45}, {9, 0.3}, {14, 0.12}}),
                            sparse({{1, 1.0}, {2, 0.08}, {6, 0.35}, {13, 0.25}})},
                           kFrames),
                true);
}

// Bell: a church bell (hum, prime, minor-third tierce, quint, nominal and the inharmonic upper
// partials), soft -> bright, then -> a clangy metal plate. The partials need room to sit off the
// harmonic series, so the prime is harmonic 16: play it four octaves down (Octave -3, Semi -12).
void makeBell(Wavetable& t, const Plans& p) {
    const std::vector<Partial> bell = {{8, 0.6},   {16, 0.7}, {19, 0.8}, {24, 0.35}, {32, 1.0},  {40, 0.45}, {43, 0.4},
                                       {48, 0.3},  {64, 0.3}, {71, 0.22}, {83, 0.2}, {101, 0.14}, {117, 0.1}, {139, 0.06}};
    const std::vector<Partial> metal = {{16, 0.6}, {23, 0.9}, {29, 0.7},  {37, 0.8},  {46, 0.6},  {53, 0.5}, {67, 0.55},
                                        {79, 0.4}, {97, 0.35}, {113, 0.3}, {131, 0.2}, {157, 0.15}, {181, 0.1}};
    std::vector<Spectrum> specs;
    for (int f = 0; f < kFrames; ++f) {   // soft -> bright: the upper partials come in
        const double dark = 1.2 * (1.0 - at(f, kFrames));
        specs.push_back(sparse(bell, [dark](int h) { return h <= 16 ? 1.0 : std::exp(-(h / 16.0 - 1.0) * dark); }));
    }
    const Spectrum bright = sparse(bell), plate = sparse(metal);
    for (int f = 1; f <= kFrames; ++f) specs.push_back(mix(bright, plate, static_cast<double>(f) / kFrames));
    addSpectra(t, p, specs, true);
}

// Browser order: the four originals (Classic first: it is the fallback), then by family.
struct Recipe {
    const char* name;
    void (*make)(Wavetable&, const Plans&);
};
constexpr Recipe kRecipes[] = {
    {"Classic", makeClassic},       {"PWM", makePwm},           {"Sync", makeSync},
    {"Formant", makeFormant},       {"Square Sync", makeSquareSync}, {"Reso Saw", makeResoSaw},
    {"Reso Square", makeResoSquare}, {"Harmonics", makeHarmonics}, {"Comb Saw", makeCombSaw},
    {"Fold", makeFold},             {"Phase Dist", makePhaseDist}, {"CZ Reso", makeCzReso},
    {"FM Ratio 1", makeFm1},        {"FM Ratio 2", makeFm2},     {"FM Ratio 3", makeFm3},
    {"FM Tine", makeFmTine},        {"Digital", makeDigital},    {"Bitcrush", makeBitcrush},
    {"Chip", makeChip},             {"Vowels", makeVowels},      {"Choir", makeChoir},
    {"Growl", makeGrowl},           {"Organ", makeOrgan},        {"E-Piano", makeEPiano},
    {"Strings", makeStrings},       {"Brass", makeBrass},        {"Reed", makeReed},
    {"Pluck", makePluck},           {"Mallet", makeMallet},      {"Bell", makeBell},
};
constexpr int kNumBuiltins = static_cast<int>(sizeof kRecipes / sizeof kRecipes[0]);

struct Tables {
    Wavetable classicBuiltin;
    Wavetable classic[CW_COUNT];
};

Tables build() {
    const Plans plans;
    Tables out;
    out.classicBuiltin.name = kRecipes[0].name;
    kRecipes[0].make(out.classicBuiltin, plans);

    // The classic oscillator shapes. Not normalised per frame: a square is louder than a
    // sine, as on an analog synth. Saw and square peak above 1 by their Gibbs overshoot only.
    const char* names[CW_COUNT] = {"Sine", "Triangle", "Saw", "Square", "Pulse"};
    const Spectrum shapes[4] = {sine(), triangle(), saw(), square()};
    for (int w = 0; w < CW_PULSE; ++w) {
        out.classic[w].name = names[w];
        addFrames(out.classic[w], plans, shapes[w], nullptr, false);
    }
    out.classic[CW_PULSE].name = names[CW_PULSE];
    std::vector<Spectrum> specs;
    for (int f = 0; f < kPulseFrames; ++f) specs.push_back(pulse(0.5 - 0.47 * f / (kPulseFrames - 1)));
    addSpectra(out.classic[CW_PULSE], plans, specs, false);
    return out;
}

const Tables& tables() {
    static const Tables t = build();   // thread-safe one-time init
    return t;
}

} // namespace

int builtinCount() { return kNumBuiltins; }

const char* builtinName(int i) { return i >= 0 && i < kNumBuiltins ? kRecipes[i].name : ""; }

int builtinIndex(const std::string& name) {
    for (int i = 0; i < kNumBuiltins; ++i)
        if (name == kRecipes[i].name) return i;
    return -1;
}

const Wavetable& classicBuiltin() { return tables().classicBuiltin; }

bool buildBuiltin(int i, Wavetable& out) {
    if (i < 0 || i >= kNumBuiltins) return false;
    Wavetable t;
    t.name = kRecipes[i].name;
    const Plans plans;
    kRecipes[i].make(t, plans);
    out = std::move(t);
    return true;
}

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
    if (tag == 3) {   // finite but huge (near FLT_MAX) would overflow to inf in the build: garbage anyway
        float v;
        std::memcpy(&v, p, sizeof v);
        return std::isfinite(v) ? std::max(-1e6f, std::min(v, 1e6f)) : 0.0f;
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
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return fail(err, "cannot open");
    // A long sample named *.wav must not be read whole: the largest real table (256 frames of
    // 4096 samples in 32-bit, plus headers) is about 4 MB.
    constexpr std::streamoff kMaxBytes = 32 << 20;
    const std::streamoff size = f.tellg();
    if (size < 0 || size > kMaxBytes) return fail(err, "too big for a wavetable");
    f.seekg(0);
    std::vector<uint8_t> b(static_cast<size_t>(size));
    if (size > 0 && !f.read(reinterpret_cast<char*>(b.data()), size)) return fail(err, "cannot read");
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
    if (!std::isfinite(peak)) return fail(err, "bad sample values");
    if (peak < 1e-6f) return fail(err, "silent");
    const float gain = 1.0f / peak;
    for (float& v : t.data) {
        v *= gain;
        if (!std::isfinite(v)) return fail(err, "bad sample values");   // never a NaN table on the audio thread
    }

    out = std::move(t);
    return true;
}

} // namespace pf
