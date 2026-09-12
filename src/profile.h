#pragma once
// Hardware Dispatch: 2 Lite / Ultra profiles + system policy.
//  Lite : H=96,E=24 (~42k params, INT8, SSSE3/SSE4.1/basic NEON, ~80KB,
//         skip backprop if P(byte)>90%)
//  Ultra: H=384,E=24 (~261k params ~200K class, INT16/FP32 fixed-point equivalent,
//         AVX2+FMA / NEON DotProd, ~600KB, full backprop)
#include <cstddef>
#include <cstdint>
#include <string>

namespace kzip {

enum class Profile : uint8_t { Auto = 0, Lite = 1, Ultra = 2 };

const char* profile_name(Profile p);
Profile profile_from_string(const std::string& s); // "lite","ultra","auto"

// Standard model size of each profile
void profile_hidden_embed(Profile p, int& H, int& E);
int profile_param_count(Profile p);
size_t profile_cache_bytes(Profile p); // ~80KB / ~600KB

// Auto-select profile by runtime CPU (AVX2/DotProd present -> Ultra, else Lite)
Profile auto_profile();

// Thread limit against machine overload:
//   CPU <=2 cores -> 1 thread; CPU 4 cores -> 3 threads; else min(requested, hc).
int effective_threads(int requested);

// Lower process priority: nice(10)/SCHED_BATCH (Linux), BELOW_NORMAL (Windows).
void lower_process_priority();

// Expected SIMD backend per profile (for display/log)
const char* profile_simd_expect(Profile p);

} // namespace kzip
