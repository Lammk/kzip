#pragma once
// Clean libkzip C API for other apps / plugins (stable ABI).
//   kzip_compress_stream / kzip_decompress_stream (v2 solid-aware at single-stream level)
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KZIP_METHOD_ID 0x4B5A
#define KZIP_PROFILE_AUTO 0
#define KZIP_PROFILE_LITE 1
#define KZIP_PROFILE_ULTRA 2

const char* kzip_version(void);
uint32_t kzip_method_id(void);

// Compress one independent stream. profile: 0 auto, 1 lite, 2 ultra.
// On success returns 0, *out owned by lib (free with kzip_free).
int kzip_compress_stream(const uint8_t* in, size_t in_n,
                         uint8_t** out, size_t* out_n, int profile);
// Decompress (auto-detects v1/v2/profile). Returns 0 on OK.
int kzip_decompress_stream(const uint8_t* in, size_t in_n,
                           uint8_t** out, size_t* out_n);
void kzip_free(void* p);

#ifdef __cplusplus
}
#endif
