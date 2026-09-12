#pragma once
// Minimal SHA-256, public-domain style, integers only.
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
namespace kzip {
void sha256(const uint8_t* data, size_t n, uint8_t out32[32]);
std::string sha256_hex(const uint8_t* data, size_t n);
std::string sha256_hex(const std::vector<uint8_t>& v);
} // namespace kzip
