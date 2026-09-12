#pragma once
// Fixed-point Q12 utilities: entire predict path uses integers
// ensures determinism across CPUs (no float/libm on hot path).
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace kzip { namespace fixed {

static constexpr int SHIFT = 12;
static constexpr int32_t SCALE = (1 << SHIFT); // 4096 = 1.0
static constexpr int32_t HALF = (1 << (SHIFT - 1));

// Multiply two Q12 numbers -> Q12 (round + clamp)
inline int32_t mul_q12(int32_t a, int32_t b) {
  int64_t p = (int64_t)a * (int64_t)b;
  p += (p >= 0 ? HALF : -HALF);
  p >>= SHIFT;
  if (p > INT32_MAX) return INT32_MAX;
  if (p < INT32_MIN) return INT32_MIN;
  return (int32_t)p;
}

inline int32_t clamp32(int64_t v) {
  if (v > INT32_MAX) return INT32_MAX;
  if (v < INT32_MIN) return INT32_MIN;
  return (int32_t)v;
}

inline int32_t clamp_range(int32_t v, int32_t lo, int32_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

// Hard-tanh Q12: clamp to [-SCALE, SCALE] equivalent to [-1, 1]
// Derivative: 1 if |x| <= SCALE, else 0. Used for Profile Lite (fast).
inline int32_t hard_tanh(int32_t x) {
  if (x < -SCALE) return -SCALE;
  if (x > SCALE) return SCALE;
  return x;
}
inline int32_t hard_tanh_deriv(int32_t pre) {
  return (pre >= -SCALE && pre <= SCALE) ? SCALE : 0; // 1.0 in Q12 form
}

// More accurate tanh via integer-only Pade approximation (deterministic, no libm):
//   tanh(x) ~= x*(27 + x^2)/(27 + 9*x^2), with x in float units.
// In Q12 domain: let X = x (Q12). Compute x2 = (X*X)>>12 (Q12, = x^2).
// Tu = X*(27*S + x2)/S ... mind the units. Direct implementation:
inline int32_t tanh_pade_q12(int32_t x) {
  // clamp outside [-4,4] -> +-1
  const int32_t LIM = 4 * SCALE;
  if (x >= LIM) return SCALE;
  if (x <= -LIM) return -SCALE;
  // x2 = x^2 (Q12): x in Q12, x^2 in Q24 -> >>12 = Q12 (max 16.0*4096, fits int32)
  int64_t x2 = ((int64_t)x * x) >> SHIFT; // Q12
  // num = x*(27 + x2/SCALE?) -- original Pade with float x: num = x*(27+x^2), den = 27+9x^2.
  // Convert to Q12: X=x*S. x^2 = x2/S. num_f = x*(27+x^2) -> Q: X*(27*S + x2)/S.
  // Resulting Q12 tanh = num/den *S = X*(27*S+x2)/(27*S+9*x2).
  int64_t a = 27LL * SCALE; // Q12
  int64_t num = (int64_t)x * (a + x2);
  int64_t den = a + 9 * x2; // >0
  int64_t r = num / den;    // Q12 (since x is Q12, (a+x2)/den is dimensionless)
  if (r > SCALE) r = SCALE;
  if (r < -SCALE) r = -SCALE;
  return (int32_t)r;
}

// Tanh lookup-table LUT for Profile Ultra: precomputed once with integer-only Pade,
// then O(1) lookup, linear interpolation. Deterministic (no libm on hot path).
// Range [-4,4] Q12, step 64 raw (1/64.0), 513 entries.
inline int32_t tanh_lut_q12(int32_t x) {
  const int32_t LIM = 4 * SCALE;
  if (x >= LIM) return SCALE;
  if (x <= -LIM) return -SCALE;
  static std::once_flag once;
  static std::vector<int32_t> lut; // 513 entries for [-4,4]
  static constexpr int N = 512;
  std::call_once(once, [] {
    lut.resize(N + 1);
    for (int i = 0; i <= N; ++i) {
      int32_t xv = -LIM + (int64_t)i * (2 * LIM) / N;
      lut[i] = tanh_pade_q12(xv);
    }
  });
  // position
  int64_t pos = ((int64_t)(x + LIM) * N) / (2 * LIM); // [0,N]
  int idx = (int)pos;
  if (idx < 0) idx = 0;
  if (idx >= N) return lut[N];
  int64_t base = -(int64_t)LIM + (int64_t)idx * (2 * LIM) / N;
  int64_t frac = (int64_t)x - base;
  int64_t step = (2LL * LIM) / N;
  int64_t v0 = lut[idx], v1 = lut[idx + 1];
  int64_t r = v0 + (v1 - v0) * frac / step;
  return (int32_t)r;
}
inline int32_t tanh_lut_deriv_q12(int32_t pre) {
  // derivative sech^2 = 1 - tanh^2, in Q12 form (1.0 = SCALE)
  int32_t t = tanh_lut_q12(pre);
  int64_t t2 = ((int64_t)t * t) >> SHIFT; // Q12
  int64_t d = (int64_t)SCALE - t2;
  if (d < 0) d = 0;
  return (int32_t)d;
}

// Deterministic xorshift32 PRNG for weight initialization
struct XorShift32 {
  uint32_t s;
  explicit XorShift32(uint32_t seed = 1337) : s(seed ? seed : 1337u) {}
  uint32_t next() {
    uint32_t x = s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s = x;
    return x;
  }
  // returns [-bound, bound] (Q12 raw int)
  int32_t next_range(int32_t bound) {
    uint32_t r = next();
    int64_t v = (int64_t)(r % (uint32_t)(2 * bound + 1)) - bound;
    return (int32_t)v;
  }
};

}} // namespace kzip::fixed
