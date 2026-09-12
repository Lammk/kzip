#include "rans.h"

namespace kzip { namespace rans {

void build_cum(const uint16_t freq[256], uint32_t cum[257]) {
  cum[0] = 0;
  for (int i = 0; i < 256; ++i) cum[i + 1] = cum[i] + freq[i];
}

Encoder::Encoder() : x_(RANS_L) {
  out_.reserve(1 << 14);
}

void Encoder::put_fast(uint8_t sym, uint32_t freq_s, uint32_t cum_s) {
  // renorm: while x >= freq*2^15 emit the low byte
  uint32_t x_max = freq_s << 15; // freq * 32768, freq>=1 so no overflow (max 2^31)
  // At most 2 bytes are emitted (since x < 2^31)
  while (x_ >= x_max) {
    out_.push_back((uint8_t)(x_ & 0xFF));
    x_ >>= 8;
  }
  // C(s,x) = (x / f)*M + cum + (x % f)
  uint32_t q = x_ / freq_s;
  uint32_t r = x_ % freq_s;
  x_ = q * PROB_SCALE + cum_s + r;
}

void Encoder::put(uint8_t sym, const uint16_t freq[256], const uint32_t cum[257]) {
  put_fast(sym, freq[sym], cum[sym]);
}

std::vector<uint8_t> Encoder::finish() {
  // Append 4-byte LE state at the END
  out_.push_back((uint8_t)(x_ & 0xFF));
  out_.push_back((uint8_t)((x_ >> 8) & 0xFF));
  out_.push_back((uint8_t)((x_ >> 16) & 0xFF));
  out_.push_back((uint8_t)((x_ >> 24) & 0xFF));
  return out_;
}

Decoder::Decoder(const uint8_t* data, size_t n) {
  if (n < 4) { ok_ = false; return; }
  size_t rn = n - 4;
  beg_ = data;
  ptr_ = data + rn; // point at the last renorm byte (read backwards)
  const uint8_t* s = data + rn;
  x_ = (uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16) |
       ((uint32_t)s[3] << 24);
  ok_ = true;
}

uint32_t Decoder::find_sym(uint32_t slot, const uint32_t cum[257]) {
  // Binary search: cum[s] <= slot < cum[s+1]
  int lo = 0, hi = 255;
  while (lo < hi) {
    int mid = (lo + hi + 1) >> 1;
    if (cum[mid] <= slot) lo = mid;
    else hi = mid - 1;
  }
  return (uint32_t)lo;
}

uint8_t Decoder::get(const uint16_t freq[256], const uint32_t cum[257]) {
  uint32_t slot = x_ & (PROB_SCALE - 1);
  uint32_t s = find_sym(slot, cum);
  uint32_t f = freq[s];
  uint32_t c = cum[s];
  // x_prev = f*(x>>16) + slot - c
  x_ = f * (x_ >> PROB_BITS) + slot - c;
  // renorm: while x < L refill bytes
  while (x_ < RANS_L) {
    if (ptr_ == beg_) { ok_ = false; break; } // out of data (only if the stream is corrupt)
    --ptr_;
    // Read backwards: the last emitted byte is refilled first? Since the encoder pushes in order,
    // the decoder reads back from the end toward the start.
    // In practice: encoder pushes b0,b1,...; the decoder must read ...b1,b0 in reverse.
    // ptr_ starts at the end of the renorm region, --ptr_ takes the last byte first -> correct.
    x_ = (x_ << 8) | (*ptr_);
  }
  return (uint8_t)s;
}

uint8_t Decoder::get_fast(uint32_t& slot_out, const uint32_t cum[257]) {
  uint32_t slot = x_ & (PROB_SCALE - 1);
  slot_out = slot;
  uint32_t s = find_sym(slot, cum);
  return (uint8_t)s;
}

uint8_t Decoder::get_with_slot(uint32_t slot, const uint16_t freq[256],
                               const uint32_t cum[257]) {
  uint32_t s = find_sym(slot, cum);
  uint32_t f = freq[s];
  uint32_t c = cum[s];
  x_ = f * (x_ >> PROB_BITS) + slot - c;
  while (x_ < RANS_L) {
    if (ptr_ == beg_) { ok_ = false; break; }
    --ptr_;
    x_ = (x_ << 8) | (*ptr_);
  }
  return (uint8_t)s;
}

}} // namespace kzip::rans
