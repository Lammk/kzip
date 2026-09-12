#pragma once
// Micro-Predictor: minimal fixed-point Q12 RNN + per-byte Online SGD.
//   h_{t+1} = act(Wx * emb[x_t] + Wh * h_t + bh), act = hard-tanh (Lite) / tanh LUT (Ultra)
//   logits  = Wo * h_t + bo  -> freq[256] summing to 65536 (integer linear mapping)
// Entire predict/update uses integers only -> deterministic cross-CPU.
// Unified v2 seed = 1337 (v1 legacy = 0x4B5A5A4B).
#include <cstdint>
#include <vector>

namespace kzip {

enum class PredProfile : uint8_t { Lite = 1, Ultra = 2 };

class MicroPredictor {
 public:
  // legacy levels 1..9 (v1, old seed) + current profile (v2, seed 1337)
  explicit MicroPredictor(int level = 1, uint32_t seed = 0x4B5A5A4Bu);
  MicroPredictor(int hidden, int embed, uint32_t seed);
  // Use Lite/Ultra profile directly
  static MicroPredictor for_profile(PredProfile p, uint32_t seed = 1337);

  void set_profile(PredProfile p) { prof_ = p; }
  PredProfile profile() const { return prof_; }

  void reset();
  void reset_weights(uint32_t seed);

  void predict(uint16_t freq[256]) const;
  // Returns P(byte) Q16 (freq/65536) for the upper-layer skip decision (Lite >90%)
  uint16_t predict_prob_of(uint8_t b) const;
  void update(uint8_t true_byte);

  int hidden() const { return H_; }
  int embed_dim() const { return E_; }
  int param_count() const;

 private:
  int H_ = 32, E_ = 8;
  PredProfile prof_ = PredProfile::Lite;
  bool legacy_v1_ = false; // true: keep 0.1.0 logic as-is (old clamp, no skip)
  std::vector<int32_t> emb_; // [256*E]
  std::vector<int32_t> Wx_;  // [H*E]
  std::vector<int32_t> Wh_;  // [H*H]
  std::vector<int32_t> Wo_;  // [256*H]
  std::vector<int32_t> bh_;  // [H]
  std::vector<int32_t> bo_;  // [256]
  std::vector<int32_t> h_;   // [H] current hidden Q12

  mutable std::vector<int32_t> tmp_pre_;   // [H]
  mutable std::vector<int32_t> tmp_logits_; // [256]

  void init_weights(uint32_t seed);
  void forward_hidden(const int32_t* h_in, uint8_t byte, int32_t* h_out,
                      int32_t* pre_out) const;
  void logits_from_hidden(const int32_t* h_in, int32_t* logits) const;
  static void logits_to_freq(const int32_t* logits, uint16_t freq[256]);
  int32_t activate(int32_t pre) const;
  int32_t activate_deriv(int32_t pre) const;
};

int param_count_for_hidden_embed(int H, int E);
void level_to_hidden_embed(int level, int& H, int& E);

} // namespace kzip
