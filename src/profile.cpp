#include "profile.h"
#include "simd_matvec.h"
#include <algorithm>
#include <cctype>
#include <thread>
#if defined(__linux__)
#include <sched.h>
#include <sys/resource.h>
#endif
#if !defined(_WIN32)
#include <unistd.h>
#endif
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace kzip {

const char* profile_name(Profile p) {
  switch (p) {
    case Profile::Lite: return "lite";
    case Profile::Ultra: return "ultra";
    default: return "auto";
  }
}

Profile profile_from_string(const std::string& s) {
  std::string t = s;
  for (auto& c : t) c = (char)tolower((unsigned char)c);
  if (t == "lite" || t == "l") return Profile::Lite;
  if (t == "ultra" || t == "u") return Profile::Ultra;
  return Profile::Auto;
}

void profile_hidden_embed(Profile p, int& H, int& E) {
  if (p == Profile::Ultra) { H = 384; E = 24; return; }
  if (p == Profile::Lite) { H = 96; E = 24; return; }
  // Auto: resolved to Lite/Ultra by the caller; default Lite to stay light
  H = 96; E = 24;
}

int profile_param_count(Profile p) {
  int H, E;
  profile_hidden_embed(p == Profile::Auto ? Profile::Lite : p, H, E);
  if (p == Profile::Auto) {
    // report the middle range for display
    return 256 * E + H * E + H * H + 256 * H + H + 256;
  }
  return 256 * E + H * E + H * H + 256 * H + H + 256;
}

size_t profile_cache_bytes(Profile p) {
  if (p == Profile::Ultra) return 600u << 10;
  return 80u << 10;
}

Profile auto_profile() {
  const char* b = simd::matvec_backend_name();
  std::string s = b ? b : "scalar";
  if (s.find("avx2") != std::string::npos) return Profile::Ultra;
  if (s.find("dotprod") != std::string::npos) return Profile::Ultra;
  if (s.find("neon") != std::string::npos) {
    // ARMv8.2+ DotProd is already picked as the dotprod backend; baseline neon -> Lite
    return Profile::Lite;
  }
  // x86 with SSE4.1 but no AVX2 -> Lite; scalar -> Lite
  return Profile::Lite;
}

int effective_threads(int requested) {
  unsigned hc = std::thread::hardware_concurrency();
  if (hc == 0) hc = 4;
  int cap;
  if (hc <= 2) cap = 1;
  else if (hc == 4) cap = 3;
  else cap = (int)hc;
  // default: use up to cap (keep one core for the system when hc>4? still use all,
  // but lower priority). If the user specifies a value, honor it but cap at cap*2.
  if (requested <= 0) return cap;
  if (requested > cap * 2) requested = cap * 2;
  return requested;
}

void lower_process_priority() {
#if defined(__linux__)
  // nice(10): yield CPU; SCHED_BATCH: batch scheduling
  nice(10);
  struct sched_param sp{};
  sp.sched_priority = 0;
  sched_setscheduler(0, SCHED_BATCH, &sp);
#elif defined(_WIN32)
  SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#else
  // generic macOS/POSIX: nice(10)
  nice(10);
#endif
}

const char* profile_simd_expect(Profile p) {
  if (p == Profile::Ultra) {
#if defined(__aarch64__)
    return "NEON DotProd";
#else
    return "AVX2+FMA";
#endif
  }
#if defined(__aarch64__)
  return "NEON";
#else
  return "SSSE3/SSE4.1";
#endif
}

} // namespace kzip
