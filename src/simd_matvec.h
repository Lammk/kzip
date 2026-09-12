#pragma once
// Fixed-point Q12 matrix-vector multiply with Dynamic Dispatch:
// scalar / SSSE3-SSE4.1 (Lite x86) / AVX2+FMA (Ultra x86) / NEON (Lite ARM)
// / NEON DotProd (Ultra ARMv8.2+).
// out[r] = (sum_c mat[r*cols+c] * vec[c] + rounding) >> SHIFT
// All backends accumulate in fixed order -> bit-identical.
#include <cstddef>
#include <cstdint>

namespace kzip { namespace simd {

using MatVecFn = void (*)(const int32_t* mat, const int32_t* vec,
                          int32_t* out, int rows, int cols);

void matvec_scalar(const int32_t* mat, const int32_t* vec,
                   int32_t* out, int rows, int cols);

// Each backend (may fall back to scalar if the build lacks support)
void matvec_sse41(const int32_t* mat, const int32_t* vec,
                  int32_t* out, int rows, int cols);
void matvec_avx2(const int32_t* mat, const int32_t* vec,
                 int32_t* out, int rows, int cols);
void matvec_avx2_fma(const int32_t* mat, const int32_t* vec,
                     int32_t* out, int rows, int cols);
void matvec_neon(const int32_t* mat, const int32_t* vec,
                 int32_t* out, int rows, int cols);
void matvec_neon_dotprod(const int32_t* mat, const int32_t* vec,
                         int32_t* out, int rows, int cols);

// Runtime CPU capabilities
bool cpu_supports_sse41();
bool cpu_supports_avx2();
bool cpu_supports_fma();
bool cpu_supports_neon_dotprod();

// Pick the optimal function (default, Ultra equivalent if available, else Lite scalar)
MatVecFn pick_matvec();
const char* matvec_backend_name();

// Pick by profile: Lite -> sse41/neon-basic, Ultra -> avx2_fma/neon_dotprod
MatVecFn pick_matvec_for_profile(int profile_id); // 1=Lite, 2=Ultra
const char* matvec_backend_for_profile(int profile_id);

}} // namespace kzip::simd
