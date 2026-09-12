// Pre-filter gate test: magic O(1) + entropy 7.92 LUT.
// (Do not use CHECK() for tasks with side effects: NDEBUG would remove them.)
#include "../src/prefilter.h"
#include <cstdio>
#include <vector>

#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL line %d: %s\n", __LINE__, #cond); return 1; } } while (0)

int main() {
  // 1. Magic bytes -> STORE
  uint8_t png[8] = {0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A};
  auto v = kzip::prefilter::analyze(png, 8);
  CHECK(v.should_store);
  printf("png: store=1 (%s) H=%.3f\n", v.reason, v.entropy);

  uint8_t zip[4] = {'P','K',0x03,0x04};
  v = kzip::prefilter::analyze(zip, 4);
  CHECK(v.should_store);
  printf("zip: store=1 (%s)\n", v.reason);

  uint8_t zstd[4] = {0x28,0xB5,0x2F,0xFD};
  v = kzip::prefilter::analyze(zstd, 4);
  CHECK(v.should_store);
  printf("zstd: store=1 (%s)\n", v.reason);

  // 2. Low-entropy text -> COMPRESS
  std::vector<uint8_t> txt(20000, 'a');
  v = kzip::prefilter::analyze(txt.data(), txt.size());
  CHECK(!v.should_store && v.entropy < 1.0);
  printf("txt: store=0 H=%.3f\n", v.entropy);

  // 3. Random ~8.0 -> STORE (threshold 7.92)
  unsigned s = 999;
  std::vector<uint8_t> rnd(70000);
  for (auto& b : rnd) { s = s*1664525u+1013904223u; b = (uint8_t)((s>>16)&0xFF); }
  v = kzip::prefilter::analyze(rnd.data(), rnd.size());
  printf("rnd: store=%d H=%.3f sampled=%zu\n", v.should_store, v.entropy, v.sampled);
  CHECK(v.should_store && v.entropy >= 7.92);

  // 4. Extension hint
  CHECK(kzip::prefilter::extension_likely_compressed("a.JPG"));
  CHECK(!kzip::prefilter::extension_likely_compressed("b.cpp"));
  printf("PREFILTER-OK\n");
  return 0;
}
