#include "../include/kzip/kzip.h"
#include <cstdio>
#include <iostream>
#include <random>
#include <vector>

// Round-trip 1MB: Data -> Compress -> Decompress == Data, SHA-256 matches 100%.
int main() {
  const size_t N = 1 << 20;
  std::vector<uint8_t> data(N);
  // Mixed data: 512KB repeated text + 512KB structured pseudo-random data
  // (so the predictor can learn, yet it stays hard).
  std::mt19937 rng(0x4B5A);
  const char* words[] = {"kzip ", "compress ", "data ", "lossless ", "rANS ",
                         "predictor ", "fixed-point ", "chunk ", "SIMD ", "ZIP "};
  size_t pos = 0;
  while (pos < 512 * 1024) {
    const char* w = words[rng() % 10];
    for (const char* p = w; *p && pos < 512 * 1024; ++p) data[pos++] = (uint8_t)*p;
  }
  while (pos < N) {
    // Simple Markov: repeat a previous byte with p=0.7
    if (pos > 0 && (rng() % 10) < 7) data[pos] = data[pos - 1 - (rng() % 8)];
    else data[pos] = (uint8_t)(rng() & 0xFF);
    pos++;
  }
  std::string h1 = kzip::sha256_hex(data.data(), data.size());
  std::cout << "orig sha256: " << h1 << "\n";
  std::cout << "params(level1): ~" << kzip::param_count_for_level(1) << "\n";

  std::vector<uint8_t> comp;
  if (!kzip::compress_buffer(data.data(), data.size(), comp, 1, 2)) {
    std::cerr << "compress failed\n";
    return 1;
  }
  std::cout << "comp: " << data.size() << " -> " << comp.size()
            << " (" << 100.0 * comp.size() / data.size() << "%)\n";

  std::vector<uint8_t> dec;
  if (!kzip::decompress_buffer(comp.data(), comp.size(), dec)) {
    std::cerr << "decompress failed\n";
    return 1;
  }
  std::string h2 = kzip::sha256_hex(dec.data(), dec.size());
  std::cout << "dec  sha256: " << h2 << "\n";
  if (h1 != h2 || dec != data) {
    std::cerr << "FAIL: mismatch\n";
    return 1;
  }
  std::cout << "PASS: round-trip 1MB lossless 100%\n";
  return 0;
}
