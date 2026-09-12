#include "chunk.h"
#include "predictor.h"
#include "rans.h"
#include "../include/kzip/config.h"
#include <cstring>
#include <vector>

namespace kzip {

static inline void put_u32le(std::vector<uint8_t>& o, uint32_t v) {
  o.push_back((uint8_t)(v & 0xFF));
  o.push_back((uint8_t)((v >> 8) & 0xFF));
  o.push_back((uint8_t)((v >> 16) & 0xFF));
  o.push_back((uint8_t)((v >> 24) & 0xFF));
}
static inline uint32_t get_u32le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

std::vector<uint8_t> compress_chunk_with_pred(const uint8_t* data, size_t n,
                                              MicroPredictor& pred) {
  std::vector<uint8_t> out;
  if (n == 0) { put_u32le(out, 0); return out; }
  size_t num_slices = (n + kSliceSize - 1) / kSliceSize;
  put_u32le(out, (uint32_t)num_slices);
  for (size_t s = 0; s < num_slices; ++s) {
    size_t off = s * kSliceSize;
    size_t len = n - off < kSliceSize ? n - off : kSliceSize;
    // Reuse a thread-local 8MB scratch buffer instead of allocating per slice.
    thread_local std::vector<uint16_t> freqs;
    freqs.resize(len * 256);
    for (size_t i = 0; i < len; ++i) {
      uint16_t f[256];
      pred.predict(f);
      memcpy(&freqs[i * 256], f, sizeof(f));
      pred.update(data[off + i]);
    }
    rans::Encoder enc;
    uint32_t cum[257];
    for (size_t k = len; k-- > 0;) {
      const uint16_t* f = &freqs[k * 256];
      cum[0] = 0;
      for (int j = 0; j < 256; ++j) cum[j + 1] = cum[j] + f[j];
      enc.put(data[off + k], f, cum);
    }
    std::vector<uint8_t> stream = enc.finish();
    put_u32le(out, (uint32_t)stream.size());
    out.insert(out.end(), stream.begin(), stream.end());
  }
  return out;
}

bool decompress_chunk_with_pred(const uint8_t* comp, size_t comp_n,
                                const uint8_t* /*raw_hint*/, size_t raw_size,
                                MicroPredictor& pred,
                                std::vector<uint8_t>& raw_out) {
  raw_out.clear();
  raw_out.resize(raw_size);
  if (raw_size == 0) return true;
  if (comp_n < 4) return false;
  uint32_t num_slices = get_u32le(comp);
  comp += 4; comp_n -= 4;
  size_t off = 0;
  for (uint32_t s = 0; s < num_slices; ++s) {
    if (comp_n < 4) return false;
    uint32_t slen = get_u32le(comp);
    comp += 4; comp_n -= 4;
    if (comp_n < slen) return false;
    size_t slice_off = off;
    size_t slice_len = raw_size - slice_off < kSliceSize ? raw_size - slice_off : kSliceSize;
    rans::Decoder dec(comp, slen);
    if (!dec.ok()) return false;
    for (size_t i = 0; i < slice_len; ++i) {
      uint16_t f[256];
      pred.predict(f);
      uint32_t cum[257];
      cum[0] = 0;
      for (int j = 0; j < 256; ++j) cum[j + 1] = cum[j] + f[j];
      uint8_t b = dec.get(f, cum);
      if (!dec.ok() && i + 1 < slice_len) return false;
      raw_out[slice_off + i] = b;
      pred.update(b);
    }
    comp += slen; comp_n -= slen;
    off += slice_len;
  }
  return off == raw_size;
}

// --- v1 wrappers (one fresh predictor per chunk, legacy seed) ---
std::vector<uint8_t> compress_one_chunk(const uint8_t* data, size_t n, int level) {
  MicroPredictor pred(level, kSeedV1);
  return compress_chunk_with_pred(data, n, pred);
}

bool decompress_one_chunk(const uint8_t* comp, size_t comp_n,
                          std::vector<uint8_t>& raw_out, size_t raw_size,
                          int level) {
  MicroPredictor pred(level, kSeedV1);
  return decompress_chunk_with_pred(comp, comp_n, nullptr, raw_size, pred, raw_out);
}

} // namespace kzip
