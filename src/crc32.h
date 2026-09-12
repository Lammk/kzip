#pragma once
#include <cstdint>
#include <cstddef>
namespace kzip {
uint32_t crc32_compute(const uint8_t* data, size_t n);
uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t n);
} // namespace kzip
