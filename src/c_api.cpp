#include "../include/kzip/c_api.h"
#include "../include/kzip/kzip.h"
#include "container.h"
#include "profile.h"
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {

const char* kzip_version(void) { return KZIP_VERSION_STRING; }
uint32_t kzip_method_id(void) { return 0x4B5A; }

int kzip_compress_stream(const uint8_t* in, size_t in_n,
                         uint8_t** out, size_t* out_n, int profile) {
  if (!out || !out_n) return -1;
  *out = nullptr; *out_n = 0;
  kzip::container::V2Params vp;
  if (profile == KZIP_PROFILE_ULTRA) {
    int H, E;
    kzip::profile_hidden_embed(kzip::Profile::Ultra, H, E);
    vp.H = H; vp.E = E; vp.profile = 2;
  } else if (profile == KZIP_PROFILE_LITE) {
    int H, E;
    kzip::profile_hidden_embed(kzip::Profile::Lite, H, E);
    vp.H = H; vp.E = E; vp.profile = 1;
  } else {
    kzip::Profile p = kzip::auto_profile();
    int H, E;
    kzip::profile_hidden_embed(p, H, E);
    vp.H = H; vp.E = E; vp.profile = (p == kzip::Profile::Ultra) ? 2 : 1;
  }
  vp.seed = kzip::kSeedV2;
  std::vector<uint8_t> s = kzip::container::encode_stream_v2(
      in_n ? in : nullptr, in_n, vp, nullptr, 0, nullptr);
  uint8_t* buf = (uint8_t*)malloc(s.size() ? s.size() : 1);
  if (!buf) return -1;
  if (!s.empty()) memcpy(buf, s.data(), s.size());
  *out = buf; *out_n = s.size();
  return 0;
}

int kzip_decompress_stream(const uint8_t* in, size_t in_n,
                           uint8_t** out, size_t* out_n) {
  if (!out || !out_n) return -1;
  *out = nullptr; *out_n = 0;
  if (!in && in_n) return -1;
  std::vector<uint8_t> raw;
  bool ok = false;
  if (in_n >= 4 && in[0]=='K' && in[1]=='Z' && in[2]==0x02) {
    kzip::container::V2Params pr;
    ok = kzip::container::decode_stream_v2(in, in_n, raw, &pr,
                                           nullptr, nullptr, nullptr, nullptr, nullptr);
  } else if (in_n >= 4 && in[0]=='K' && in[1]=='Z' && in[2]==0x01) {
    ok = kzip::container::decode_stream(in, in_n, raw);
  } else {
    return -1;
  }
  if (!ok) return -1;
  uint8_t* buf = (uint8_t*)malloc(raw.size() ? raw.size() : 1);
  if (!buf) return -1;
  if (!raw.empty()) memcpy(buf, raw.data(), raw.size());
  *out = buf; *out_n = raw.size();
  return 0;
}

void kzip_free(void* p) { free(p); }

} // extern "C"
