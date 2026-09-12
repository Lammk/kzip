#pragma once
// Chunking Engine:
//  v1 legacy: 1MB split, each chunk resets predictor (old seed), independent parallel.
//  v2 solid : continuous predictor across chunks in one solid group (seed 1337),
//             per-chunk CRC32, 16KB rANS slices.
#include <cstdint>
#include <vector>

namespace kzip {

class MicroPredictor;

// --- v1 (kept as-is to read old archives) ---
std::vector<uint8_t> compress_one_chunk(const uint8_t* data, size_t n, int level);
bool decompress_one_chunk(const uint8_t* comp, size_t comp_n,
                          std::vector<uint8_t>& raw_out, size_t raw_size,
                          int level = 1);

// --- v2: use external predictor (continuous), returns chunk payload ---
// Chunk payload format (shared v1/v2):
// [u32 num_slices] + per slice [u32 comp_size][rans stream...]
std::vector<uint8_t> compress_chunk_with_pred(const uint8_t* data, size_t n,
                                              MicroPredictor& pred);
bool decompress_chunk_with_pred(const uint8_t* comp, size_t comp_n,
                                const uint8_t* raw_hint, size_t raw_size,
                                MicroPredictor& pred,
                                std::vector<uint8_t>& raw_out);
} // namespace kzip
