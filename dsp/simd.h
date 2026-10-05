#pragma once
// Four floats at a time: GCC vector types, which become NEON q-register code on the Force
// (-mfpu=neon-vfpv4 -funsafe-math-optimizations) and SSE on x86, so the tests run the same
// code as the device. ARM intrinsics only where the generic form compiles badly (min/max,
// the reciprocal: ARMv7 NEON has no divide).
#include <cstring>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define PF_NEON 1
#endif

namespace pf {

typedef float f4 __attribute__((vector_size(16)));

inline f4 splat(float x) { return f4{x, x, x, x}; }
inline f4 load4(const float* p) {
    f4 v;
    __builtin_memcpy(&v, p, sizeof v);   // a plain (unaligned-safe) vector load
    return v;
}
// The builtin, not std::memcpy: with _FORTIFY_SOURCE (on by default in Ubuntu's compilers) a store
// into a local array at a variable index became a __memcpy_chk call instead of one vst1.
inline void store4(float* p, f4 v) { __builtin_memcpy(p, &v, sizeof v); }

inline f4 min4(f4 a, f4 b) {
#ifdef PF_NEON
    return (f4)vminq_f32((float32x4_t)a, (float32x4_t)b);
#else
    return a < b ? a : b;
#endif
}
inline f4 max4(f4 a, f4 b) {
#ifdef PF_NEON
    return (f4)vmaxq_f32((float32x4_t)a, (float32x4_t)b);
#else
    return a > b ? a : b;
#endif
}

// 1 / x: NEON's estimate refined twice (about 24 bits, like a divide), else a divide.
inline f4 recip4(f4 x) {
#ifdef PF_NEON
    float32x4_t e = vrecpeq_f32((float32x4_t)x);
    e = vmulq_f32(e, vrecpsq_f32((float32x4_t)x, e));
    e = vmulq_f32(e, vrecpsq_f32((float32x4_t)x, e));
    return (f4)e;
#else
    return splat(1.0f) / x;
#endif
}

// The sum of the four lanes.
inline float hsum4(f4 v) {
#ifdef PF_NEON
    const float32x4_t q = (float32x4_t)v;
    const float32x2_t s = vadd_f32(vget_low_f32(q), vget_high_f32(q));
    return vget_lane_f32(vpadd_f32(s, s), 0);
#else
    return (v[0] + v[1]) + (v[2] + v[3]);
#endif
}

// The sums of four vectors' lanes, as one vector: {sum a, sum b, sum c, sum d}.
inline f4 hsum4x4(f4 a, f4 b, f4 c, f4 d) {
#ifdef PF_NEON
    const float32x4_t A = (float32x4_t)a, B = (float32x4_t)b, C = (float32x4_t)c, D = (float32x4_t)d;
    const float32x2_t ab = vpadd_f32(vadd_f32(vget_low_f32(A), vget_high_f32(A)), vadd_f32(vget_low_f32(B), vget_high_f32(B)));
    const float32x2_t cd = vpadd_f32(vadd_f32(vget_low_f32(C), vget_high_f32(C)), vadd_f32(vget_low_f32(D), vget_high_f32(D)));
    return (f4)vcombine_f32(ab, cd);
#else
    return f4{hsum4(a), hsum4(b), hsum4(c), hsum4(d)};
#endif
}

// The engine's tanh-like saturator (dsp/synth.cpp softclip), four at a time.
inline f4 softclip4(f4 x) {
    x = min4(max4(x, splat(-3.0f)), splat(3.0f));
    const f4 x2 = x * x;
    return x * (splat(27.0f) + x2) * recip4(splat(27.0f) + splat(9.0f) * x2);
}

} // namespace pf
