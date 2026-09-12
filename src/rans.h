#pragma once
// 32-bit Range ANS (rANS), PROB_BITS=16, total 65536, byte-wise renorm.
// MDT: L = 1<<23, base-256 output.
#include <cstdint>
#include <vector>

namespace kzip { namespace rans {

static constexpr uint32_t PROB_BITS = 16;
static constexpr uint32_t PROB_SCALE = 1u << PROB_BITS;
static constexpr uint32_t RANS_L = 1u << 23;

// Uses cum[257]: cum[0]=0, cum[256]=65536
void build_cum(const uint16_t freq[256], uint32_t cum[257]);

class Encoder {
 public:
  Encoder();
  void put(uint8_t sym, const uint16_t freq[256], const uint32_t cum[257]);
  // or pass the symbol's freq/cum directly (faster)
  void put_fast(uint8_t sym, uint32_t freq_s, uint32_t cum_s);
  std::vector<uint8_t> finish(); // flush state LE + prepend? returns renorm bytes + state
  // get current renorm bytes (internal use)
  const std::vector<uint8_t>& out() const { return out_; }
  uint32_t state() const { return x_; }
 private:
  uint32_t x_;
  std::vector<uint8_t> out_;
};

class Decoder {
 public:
  // buf: entire stream of 1 slice (renorm bytes + 4-byte state at END)
  // Convention: encoder writes renorm bytes in emission order, then appends LE state (4 bytes).
  // Decoder reads backwards: state from last 4 bytes, renorm bytes backwards.
  Decoder(const uint8_t* data, size_t n);
  bool ok() const { return ok_; }
  uint8_t get(const uint16_t freq[256], const uint32_t cum[257]);
  uint8_t get_fast(uint32_t& slot_out, const uint32_t cum[257]);
  // decode 1 symbol when slot is known
  uint8_t get_with_slot(uint32_t slot, const uint16_t freq[256], const uint32_t cum[257]);
 private:
  uint32_t x_;
  const uint8_t* beg_;
  const uint8_t* ptr_; // read backwards from end of renorm region
  bool ok_ = false;
  uint32_t find_sym(uint32_t slot, const uint32_t cum[257]);
};

}} // namespace kzip::rans
