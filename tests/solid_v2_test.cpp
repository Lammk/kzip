// Solid block + v2 stream test (fast, small data).
// Checks: scan/sort/pack 8-32MB, binary manifest, profile flag, round-trip.
// (Do not use assert() for tasks with side effects: NDEBUG would remove them.)
#include "../include/kzip/kzip.h"
#include "../include/kzip/c_api.h"
#include "../src/solid.h"
#include <cstdio>
#include <cstring>
#include <vector>

#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL line %d: %s\n", __LINE__, #cond); return 1; } } while (0)

int main() {
  // 1. Binary manifest round-trip
  std::vector<kzip::solid::ManifestEntry> m = {
    {"src/a.cpp", 100, 1700000000u, 0100644, 0x12345678},
    {"docs/note.txt", 0, 1700000001u, 0100644, 0},
  };
  std::vector<uint8_t> buf;
  kzip::solid::encode_manifest(m, buf);
  std::vector<kzip::solid::ManifestEntry> m2;
  size_t cons = 0;
  CHECK(kzip::solid::decode_manifest(buf.data(), buf.size(), m2, cons));
  CHECK(m2.size() == 2 && m2[0].path == "src/a.cpp" && m2[0].size == 100);
  CHECK(m2[1].crc == 0);
  printf("manifest: OK (%zu bytes)\n", buf.size());

  // 2. pack_groups: 8MB target / 32MB max
  std::vector<kzip::solid::ScannedFile> fs(5);
  const char* names[] = {"b.cpp","a.cpp","x.txt","y.txt","z.bin"};
  uint64_t sizes[] = {1000,1000,1000,1000,1000};
  for (int i = 0; i < 5; ++i) {
    fs[i].arcname = names[i]; fs[i].size = sizes[i];
    // simulate a real scan (scan_inputs fills lowercase ext)
    std::string a = names[i];
    size_t dot = a.find_last_of('.');
    fs[i].ext = dot == std::string::npos ? "" : a.substr(dot);
  }
  kzip::solid::sort_smart(fs);
  // sort by ext: .bin, .cpp, .cpp, .txt, .txt
  CHECK(fs[0].arcname == std::string("z.bin"));
  auto gs = kzip::solid::pack_groups(fs, 8ull<<20, 32ull<<20);
  CHECK(gs.size() == 1 && gs[0].idx.size() == 5);
  printf("solid-pack: OK (1 group, sorted by ext)\n");

  // 3. C API v2 round-trip Lite (repeated data, small for fast testing)
  std::string chunk = "solid block predictor rANS ";
  std::vector<uint8_t> raw;
  for (int i = 0; i < 200; ++i) raw.insert(raw.end(), chunk.begin(), chunk.end());
  {
    uint8_t* c = nullptr; size_t cn = 0;
    int r = kzip_compress_stream(raw.data(), raw.size(), &c, &cn, 1);
    CHECK(r == 0 && c != nullptr && cn > 100);
    CHECK(c[0]=='K' && c[1]=='Z' && c[2]==0x02);
    CHECK(c[8] == 1); // profile flag Lite in header
    uint8_t* d = nullptr; size_t dn = 0;
    int r2 = kzip_decompress_stream(c, cn, &d, &dn);
    CHECK(r2 == 0 && dn == raw.size() && memcmp(d, raw.data(), dn) == 0);
    printf("capi profile=1: %zu -> %zu OK\n", raw.size(), cn);
    kzip_free(c); kzip_free(d);
  }
  // 4. Ultra header/flag + round-trip on a very small sample (H=384)
  {
    std::vector<uint8_t> tiny(1500, 'q');
    for (size_t i = 0; i < tiny.size(); i += 31) tiny[i] = (uint8_t)(i & 0x7F);
    uint8_t* c = nullptr; size_t cn = 0;
    int r = kzip_compress_stream(tiny.data(), tiny.size(), &c, &cn, 2);
    CHECK(r == 0 && c != nullptr && cn > 50);
    CHECK(c[2] == 0x02 && c[8] == 2); // magic v2 + profile Ultra
    int H = c[4] | (c[5] << 8);
    CHECK(H == 384);
    uint8_t* d = nullptr; size_t dn = 0;
    int r2 = kzip_decompress_stream(c, cn, &d, &dn);
    CHECK(r2 == 0 && dn == tiny.size() && memcmp(d, tiny.data(), dn) == 0);
    printf("capi profile=2: %zu -> %zu OK (H=%d)\n", tiny.size(), cn, H);
    kzip_free(c); kzip_free(d);
  }
  printf("SOLID-V2-OK\n");
  return 0;
}
