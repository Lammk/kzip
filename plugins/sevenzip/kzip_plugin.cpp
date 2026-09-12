#include "kzip_plugin.h"
#include "../../include/kzip/kzip.h"
#include "../../include/kzip/c_api.h"
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#define KZIP_API __declspec(dllexport)
#else
#define KZIP_API __attribute__((visibility("default")))
#endif

namespace kzip { namespace sevenzip {

bool kzip_compress_stream(const uint8_t* in, size_t in_n,
                          std::vector<uint8_t>& out, int profile) {
  uint8_t* p = nullptr;
  size_t n = 0;
  // profile here: 0 auto, 1 lite, 2 ultra (different from v1 level)
  int prof = (profile == 1 || profile == 2) ? profile : 0;
  if (::kzip_compress_stream(in, in_n, &p, &n, prof) != 0) return false;
  out.assign(p, p + n);
  ::kzip_free(p);
  return true;
}

bool kzip_decompress_stream(const uint8_t* in, size_t in_n,
                            std::vector<uint8_t>& out, size_t) {
  // Auto-detect v1 (KZB) / v2 (KZ02). New plugin prefers v2.
  if (in_n >= 4 && in[0]=='K' && in[1]=='Z' && in[2]==0x02) {
    uint8_t* p = nullptr;
    size_t n = 0;
    if (::kzip_decompress_stream(in, in_n, &p, &n) != 0) return false;
    out.assign(p, p + n);
    ::kzip_free(p);
    return true;
  }
  // fallback v1 buffer
  return kzip::decompress_buffer(in, in_n, out);
}

}} // namespace kzip::sevenzip

// Minimal C ABI so 7-Zip / other apps can load the plugin dynamically (.dll/.so)
// Inherits ICompressCoder (7-Zip SDK): Code(in,out) will call these functions,
// registering Method ID 0x4B5A. See kzip_plugin.h to wire into the real SDK.
extern "C" {
KZIP_API uint32_t kzip_plugin_method_id() { return 0x4B5A; }
KZIP_API const char* kzip_plugin_version() { return KZIP_VERSION_STRING; }
KZIP_API int kzip_plugin_profile_lite_params() { return 96 * 24 + 256 * 24 + 96 * 96 + 256 * 96; }
KZIP_API int kzip_plugin_compress(const uint8_t* in, size_t in_n,
                                  uint8_t** out, size_t* out_n, int profile) {
  uint8_t* p = nullptr;
  size_t n = 0;
  int prof = (profile == 1 || profile == 2) ? profile : 0;
  if (::kzip_compress_stream(in, in_n, &p, &n, prof) != 0) return -1;
  *out = p; *out_n = n;
  return 0; // caller frees with kzip_plugin_free (=kzip_free)
}
KZIP_API int kzip_plugin_decompress(const uint8_t* in, size_t in_n,
                                    uint8_t** out, size_t* out_n) {
  uint8_t* p = nullptr;
  size_t n = 0;
  // Try v2 first, v1 buffer fallback
  if (::kzip_decompress_stream(in, in_n, &p, &n) == 0) {
    *out = p; *out_n = n;
    return 0;
  }
  auto* v = new std::vector<uint8_t>();
  if (!kzip::decompress_buffer(in, in_n, *v)) { delete v; return -1; }
  // Convert to malloc for a consistent ABI
  uint8_t* q = (uint8_t*)malloc(v->size() ? v->size() : 1);
  if (q && !v->empty()) memcpy(q, v->data(), v->size());
  *out = q; *out_n = v->size();
  delete v;
  return 0;
}
KZIP_API void kzip_plugin_free(void* p) { ::kzip_free(p); }
}
