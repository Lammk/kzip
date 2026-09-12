#include "crc32.h"

namespace kzip {

static uint32_t kCrcTable[256];
static bool kCrcInit = false;

static void crc_init() {
  if (kCrcInit) return;
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k)
      c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    kCrcTable[i] = c;
  }
  kCrcInit = true;
}

uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t n) {
  crc_init();
  crc ^= 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i)
    crc = kCrcTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

uint32_t crc32_compute(const uint8_t* data, size_t n) {
  if (!data && n) return 0;
  crc_init();
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i)
    crc = kCrcTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

} // namespace kzip
