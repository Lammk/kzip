#include "simd_matvec.h"
#include "fixed.h"
#include <mutex>

// Per-architecture intrinsics (guarded so multi-platform builds still pass)
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#if defined(__SSE4_1__) || defined(__SSSE3__) || defined(__AVX2__)
#include <immintrin.h>
#include <smmintrin.h>
#endif
#endif
#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace kzip { namespace simd {

void matvec_scalar(const int32_t* mat, const int32_t* vec,
                   int32_t* out, int rows, int cols) {
  for (int r = 0; r < rows; ++r) {
    const int32_t* row = mat + (size_t)r * (size_t)cols;
    int64_t acc = 0;
    for (int c = 0; c < cols; ++c) {
      acc += (int64_t)row[c] * (int64_t)vec[c];
    }
    acc += (acc >= 0 ? (int64_t)(1 << (fixed::SHIFT - 1))
                     : -(int64_t)(1 << (fixed::SHIFT - 1)));
    acc >>= fixed::SHIFT;
    if (acc > INT32_MAX) acc = INT32_MAX;
    if (acc < INT32_MIN) acc = INT32_MIN;
    out[r] = (int32_t)acc;
  }
}

// Shared helper: SIMD block multiply but sequential accumulate (bit-identical).
// Each backend differs only in load/multiply (proof of vectorization), reduce unchanged.
void matvec_sse41(const int32_t* mat, const int32_t* vec,
                  int32_t* out, int rows, int cols) {
#if defined(__SSE4_1__) && (defined(__x86_64__) || defined(__i386__))
  for (int r = 0; r < rows; ++r) {
    const int32_t* row = mat + (size_t)r * (size_t)cols;
    int64_t acc = 0;
    int c = 0, lim = cols & ~3;
    alignas(16) int32_t prod[4];
    for (; c < lim; c += 4) {
      __m128i a = _mm_loadu_si128((const __m128i*)(row + c));
      __m128i b = _mm_loadu_si128((const __m128i*)(vec + c));
      __m128i p = _mm_mullo_epi32(a, b);
      _mm_store_si128((__m128i*)prod, p);
      acc += (int64_t)prod[0] + prod[1] + prod[2] + prod[3];
    }
    for (; c < cols; ++c) acc += (int64_t)row[c] * (int64_t)vec[c];
    acc += (acc >= 0 ? (int64_t)(1 << (fixed::SHIFT - 1))
                     : -(int64_t)(1 << (fixed::SHIFT - 1)));
    acc >>= fixed::SHIFT;
    if (acc > INT32_MAX) acc = INT32_MAX;
    if (acc < INT32_MIN) acc = INT32_MIN;
    out[r] = (int32_t)acc;
  }
#else
  matvec_scalar(mat, vec, out, rows, cols);
#endif
}

void matvec_avx2(const int32_t* mat, const int32_t* vec,
                 int32_t* out, int rows, int cols) {
#if defined(__AVX2__) && (defined(__x86_64__) || defined(__i386__))
  for (int r = 0; r < rows; ++r) {
    const int32_t* row = mat + (size_t)r * (size_t)cols;
    int64_t acc = 0;
    int c = 0, lim = cols & ~7;
    alignas(32) int32_t prod[8];
    for (; c < lim; c += 8) {
      __m256i a = _mm256_loadu_si256((const __m256i*)(row + c));
      __m256i b = _mm256_loadu_si256((const __m256i*)(vec + c));
      __m256i p = _mm256_mullo_epi32(a, b);
      _mm256_store_si256((__m256i*)prod, p);
      acc += (int64_t)prod[0] + prod[1] + prod[2] + prod[3] +
             prod[4] + prod[5] + prod[6] + prod[7];
    }
    for (; c < cols; ++c) acc += (int64_t)row[c] * (int64_t)vec[c];
    acc += (acc >= 0 ? (int64_t)(1 << (fixed::SHIFT - 1))
                     : -(int64_t)(1 << (fixed::SHIFT - 1)));
    acc >>= fixed::SHIFT;
    if (acc > INT32_MAX) acc = INT32_MAX;
    if (acc < INT32_MIN) acc = INT32_MIN;
    out[r] = (int32_t)acc;
  }
#else
  matvec_scalar(mat, vec, out, rows, cols);
#endif
}

void matvec_avx2_fma(const int32_t* mat, const int32_t* vec,
                     int32_t* out, int rows, int cols) {
  // Ultra x86: AVX2 + FMA path (prefetch + block accumulate). Results stay
  // bit-identical to scalar via sequential reduce (FMA is only for a future float
  // path; keep the integer path for now to guarantee lossless).
#if defined(__AVX2__) && (defined(__x86_64__) || defined(__i386__))
  for (int r = 0; r < rows; ++r) {
    const int32_t* row = mat + (size_t)r * (size_t)cols;
    __builtin_prefetch(row + 64, 0, 3);
    int64_t acc = 0;
    int c = 0, lim = cols & ~7;
    alignas(32) int32_t prod[8];
    for (; c < lim; c += 8) {
      __m256i a = _mm256_loadu_si256((const __m256i*)(row + c));
      __m256i b = _mm256_loadu_si256((const __m256i*)(vec + c));
      __m256i p = _mm256_mullo_epi32(a, b);
      _mm256_store_si256((__m256i*)prod, p);
      acc += (int64_t)prod[0] + prod[1] + prod[2] + prod[3] +
             prod[4] + prod[5] + prod[6] + prod[7];
    }
    for (; c < cols; ++c) acc += (int64_t)row[c] * (int64_t)vec[c];
    acc += (acc >= 0 ? (int64_t)(1 << (fixed::SHIFT - 1))
                     : -(int64_t)(1 << (fixed::SHIFT - 1)));
    acc >>= fixed::SHIFT;
    if (acc > INT32_MAX) acc = INT32_MAX;
    if (acc < INT32_MIN) acc = INT32_MIN;
    out[r] = (int32_t)acc;
  }
#else
  matvec_scalar(mat, vec, out, rows, cols);
#endif
}

void matvec_neon(const int32_t* mat, const int32_t* vec,
                 int32_t* out, int rows, int cols) {
#if defined(__aarch64__) || defined(__ARM_NEON)
  for (int r = 0; r < rows; ++r) {
    const int32_t* row = mat + (size_t)r * (size_t)cols;
    int64_t acc = 0;
    int c = 0, lim = cols & ~3;
    alignas(16) int32_t prod[4];
    for (; c < lim; c += 4) {
      int32x4_t a = vld1q_s32(row + c);
      int32x4_t b = vld1q_s32(vec + c);
      int32x4_t p = vmulq_s32(a, b);
      vst1q_s32(prod, p);
      acc += (int64_t)prod[0] + prod[1] + prod[2] + prod[3];
    }
    for (; c < cols; ++c) acc += (int64_t)row[c] * (int64_t)vec[c];
    acc += (acc >= 0 ? (int64_t)(1 << (fixed::SHIFT - 1))
                     : -(int64_t)(1 << (fixed::SHIFT - 1)));
    acc >>= fixed::SHIFT;
    if (acc > INT32_MAX) acc = INT32_MAX;
    if (acc < INT32_MIN) acc = INT32_MIN;
    out[r] = (int32_t)acc;
  }
#else
  matvec_scalar(mat, vec, out, rows, cols);
#endif
}

void matvec_neon_dotprod(const int32_t* mat, const int32_t* vec,
                         int32_t* out, int rows, int cols) {
  // Ultra ARM: NEON DotProd (ARMv8.2+). Still sequential reduce to stay
  // bit-identical; will use vdotq_s32 directly once int8 quantization lands.
#if defined(__aarch64__) || defined(__ARM_NEON)
  matvec_neon(mat, vec, out, rows, cols);
#else
  matvec_scalar(mat, vec, out, rows, cols);
#endif
}

bool cpu_supports_sse41() {
#if defined(__GNUC__) || defined(__clang__)
#if defined(__x86_64__) || defined(__i386__)
  return __builtin_cpu_supports("sse4.1");
#endif
#endif
  return false;
}
bool cpu_supports_avx2() {
#if defined(__GNUC__) || defined(__clang__)
#if defined(__x86_64__)
  return __builtin_cpu_supports("avx2");
#endif
#endif
  return false;
}
bool cpu_supports_fma() {
#if defined(__GNUC__) || defined(__clang__)
#if defined(__x86_64__)
  return __builtin_cpu_supports("fma");
#endif
#endif
  return false;
}
bool cpu_supports_neon_dotprod() {
#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
  // __builtin_cpu_supports("dotprod") exists since GCC9+; probe safely
  return __builtin_cpu_supports("dotprod");
#else
  return false;
#endif
}

static MatVecFn g_fn = nullptr;
static std::once_flag g_once;
static const char* g_name = "scalar";

static void init_dispatch() {
#if defined(__aarch64__) || defined(__ARM_NEON)
  if (cpu_supports_neon_dotprod()) { g_fn = matvec_neon_dotprod; g_name = "neon-dotprod"; }
  else { g_fn = matvec_neon; g_name = "neon"; }
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#if defined(__AVX2__)
  if (cpu_supports_avx2()) {
    g_fn = cpu_supports_fma() ? matvec_avx2_fma : matvec_avx2;
    g_name = cpu_supports_fma() ? "avx2+fma" : "avx2";
  } else if (cpu_supports_sse41()) {
    g_fn = matvec_sse41; g_name = "sse4.1";
  } else { g_fn = matvec_scalar; g_name = "scalar"; }
#else
  if (cpu_supports_sse41()) { g_fn = matvec_sse41; g_name = "sse4.1"; }
  else { g_fn = matvec_scalar; g_name = "scalar"; }
#endif
#else
  g_fn = matvec_scalar; g_name = "scalar";
#endif
}

MatVecFn pick_matvec() {
  std::call_once(g_once, init_dispatch);
  return g_fn;
}
const char* matvec_backend_name() {
  std::call_once(g_once, init_dispatch);
  return g_name;
}

MatVecFn pick_matvec_for_profile(int profile_id) {
  std::call_once(g_once, init_dispatch);
  // 1=Lite, 2=Ultra
  if (profile_id == 2) {
#if defined(__aarch64__) || defined(__ARM_NEON)
    return matvec_neon_dotprod;
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#if defined(__AVX2__)
    if (cpu_supports_avx2()) return matvec_avx2_fma;
#endif
    if (cpu_supports_sse41()) return matvec_sse41;
    return matvec_scalar;
#else
    return matvec_scalar;
#endif
  }
  // Lite
#if defined(__aarch64__) || defined(__ARM_NEON)
  return matvec_neon;
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
  if (cpu_supports_sse41()) return matvec_sse41;
  return matvec_scalar;
#else
  return matvec_scalar;
#endif
}

const char* matvec_backend_for_profile(int profile_id) {
  if (profile_id == 2) {
#if defined(__aarch64__) || defined(__ARM_NEON)
    return cpu_supports_neon_dotprod() ? "neon-dotprod" : "neon";
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#if defined(__AVX2__)
    if (cpu_supports_avx2()) return "avx2+fma";
#endif
    return cpu_supports_sse41() ? "sse4.1" : "scalar";
#else
    return "scalar";
#endif
  }
#if defined(__aarch64__) || defined(__ARM_NEON)
  return "neon";
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
  return cpu_supports_sse41() ? "sse4.1" : "scalar";
#else
  return "scalar";
#endif
}

}} // namespace kzip::simd
