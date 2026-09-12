#include "predictor.h"
#include "fixed.h"
#include "profile.h"
#include "simd_matvec.h"
#include <algorithm>
#include <cstring>

namespace kzip {

void level_to_hidden_embed(int level, int& H, int& E) {
  if (level <= 1) { H = 32; E = 8; }
  else if (level == 2) { H = 48; E = 12; }
  else if (level <= 4) { H = 64; E = 16; }
  else if (level <= 6) { H = 96; E = 24; }
  else if (level <= 8) { H = 160; E = 32; }
  else { H = 224; E = 48; } // ~150k params
  if (level < 1) { H = 32; E = 8; }
  if (level > 9) { H = 224; E = 48; }
}

int param_count_for_hidden_embed(int H, int E) {
  return 256 * E + H * E + H * H + 256 * H + H + 256;
}

MicroPredictor::MicroPredictor(int level, uint32_t seed) {
  int H, E;
  level_to_hidden_embed(level, H, E);
  H_ = H; E_ = E;
  prof_ = PredProfile::Lite;
  legacy_v1_ = true; // freeze v1 behavior
  emb_.resize((size_t)256 * E_);
  Wx_.resize((size_t)H_ * E_);
  Wh_.resize((size_t)H_ * H_);
  Wo_.resize((size_t)256 * H_);
  bh_.resize(H_);
  bo_.resize(256);
  h_.assign(H_, 0);
  tmp_pre_.resize(H_);
  tmp_logits_.resize(256);
  init_weights(seed);
}

MicroPredictor::MicroPredictor(int hidden, int embed, uint32_t seed)
    : H_(hidden), E_(embed), prof_(PredProfile::Lite), legacy_v1_(false) {
  if (H_ < 8) H_ = 8;
  if (H_ > 512) H_ = 512;
  if (E_ < 4) E_ = 4;
  if (E_ > 64) E_ = 64;
  if (H_ >= 256) prof_ = PredProfile::Ultra;
  emb_.resize((size_t)256 * E_);
  Wx_.resize((size_t)H_ * E_);
  Wh_.resize((size_t)H_ * H_);
  Wo_.resize((size_t)256 * H_);
  bh_.resize(H_);
  bo_.resize(256);
  h_.assign(H_, 0);
  tmp_pre_.resize(H_);
  tmp_logits_.resize(256);
  init_weights(seed);
}

MicroPredictor MicroPredictor::for_profile(PredProfile p, uint32_t seed) {
  int H, E;
  profile_hidden_embed(p == PredProfile::Ultra ? Profile::Ultra : Profile::Lite, H, E);
  MicroPredictor m(H, E, seed);
  m.prof_ = p;
  // INT8 quantization (Lite): tighter clamp to fit int8 [-120,120]
  // (Ultra keeps full INT16 range).
  return m;
}

int MicroPredictor::param_count() const {
  return param_count_for_hidden_embed(H_, E_);
}

void MicroPredictor::init_weights(uint32_t seed) {
  fixed::XorShift32 rng(seed ? seed : 1337u);
  const int32_t b_emb = 96; // ~0.023
  const int32_t b_wx = 64;
  const int32_t b_wh = 48;
  const int32_t b_wo = 48;
  for (auto& v : emb_) v = rng.next_range(b_emb);
  for (auto& v : Wx_) v = rng.next_range(b_wx);
  for (auto& v : Wh_) v = rng.next_range(b_wh);
  for (auto& v : Wo_) v = rng.next_range(b_wo);
  for (auto& v : bh_) v = 0;
  for (auto& v : bo_) v = 0;
  std::fill(h_.begin(), h_.end(), 0);
}

void MicroPredictor::reset() {
  std::fill(h_.begin(), h_.end(), 0);
}

void MicroPredictor::reset_weights(uint32_t seed) {
  init_weights(seed);
}

int32_t MicroPredictor::activate(int32_t pre) const {
  if (prof_ == PredProfile::Ultra) return fixed::tanh_lut_q12(pre);
  return fixed::hard_tanh(pre);
}

int32_t MicroPredictor::activate_deriv(int32_t pre) const {
  if (prof_ == PredProfile::Ultra) return fixed::tanh_lut_deriv_q12(pre);
  return (pre >= -fixed::SCALE && pre <= fixed::SCALE) ? fixed::SCALE : 0;
}

void MicroPredictor::forward_hidden(const int32_t* h_in, uint8_t byte,
                                    int32_t* h_out, int32_t* pre_out) const {
  auto mv = simd::pick_matvec_for_profile(
      prof_ == PredProfile::Ultra ? 2 : 1);
  const int32_t* emb_row = emb_.data() + (size_t)byte * E_;
  mv(Wx_.data(), emb_row, pre_out, H_, E_);
  thread_local std::vector<int32_t> tls;
  if ((int)tls.size() < H_) tls.resize(H_);
  mv(Wh_.data(), h_in, tls.data(), H_, H_);
  for (int i = 0; i < H_; ++i) {
    int64_t v = (int64_t)pre_out[i] + tls[i] + bh_[i];
    if (v > INT32_MAX) v = INT32_MAX;
    if (v < INT32_MIN) v = INT32_MIN;
    int32_t pre = (int32_t)v;
    pre_out[i] = pre;
    h_out[i] = activate(pre);
  }
}

void MicroPredictor::logits_from_hidden(const int32_t* h_in,
                                        int32_t* logits) const {
  auto mv = simd::pick_matvec_for_profile(
      prof_ == PredProfile::Ultra ? 2 : 1);
  mv(Wo_.data(), h_in, logits, 256, H_);
  for (int i = 0; i < 256; ++i) {
    int64_t v = (int64_t)logits[i] + bo_[i];
    if (v > INT32_MAX) v = INT32_MAX;
    if (v < INT32_MIN) v = INT32_MIN;
    logits[i] = (int32_t)v;
  }
}

void MicroPredictor::logits_to_freq(const int32_t* logits,
                                    uint16_t freq[256]) {
  int32_t mn = logits[0];
  for (int i = 1; i < 256; ++i) if (logits[i] < mn) mn = logits[i];
  const int32_t D_MAX = 16384;
  uint64_t vals[256];
  uint64_t sum = 0;
  for (int i = 0; i < 256; ++i) {
    int64_t d = (int64_t)logits[i] - mn;
    if (d > D_MAX) d = D_MAX;
    uint64_t v = (uint64_t)(d + 32);
    vals[i] = v;
    sum += v;
  }
  uint64_t acc = 0;
  for (int i = 0; i < 256; ++i) {
    uint64_t f = (vals[i] * 65536ULL) / sum;
    if (f == 0) f = 1;
    if (f > 65536) f = 65536;
    freq[i] = (uint16_t)f;
    acc += f;
  }
  int64_t diff = (int64_t)65536 - (int64_t)acc;
  int idx = 0;
  while (diff != 0) {
    int i = idx & 255;
    if (diff > 0) { if (freq[i] < 65536 - 255) { freq[i]++; diff--; } }
    else { if (freq[i] > 1) { freq[i]--; diff++; } }
    idx++;
    if (idx > 256 * 300) break;
  }
}

void MicroPredictor::predict(uint16_t freq[256]) const {
  logits_from_hidden(h_.data(), tmp_logits_.data());
  logits_to_freq(tmp_logits_.data(), freq);
}

uint16_t MicroPredictor::predict_prob_of(uint8_t b) const {
  uint16_t f[256];
  predict(f);
  return f[b];
}

void MicroPredictor::update(uint8_t true_byte) {
  logits_from_hidden(h_.data(), tmp_logits_.data());
  uint16_t freq[256];
  logits_to_freq(tmp_logits_.data(), freq);

  const int32_t LR_WO = 10;
  const int32_t LR_BIAS = 6;
  const int32_t LR_REC = 4;

  int32_t d_q12[256];
  for (int i = 0; i < 256; ++i) {
    int32_t target = (i == true_byte) ? 65536 : 0;
    int32_t d = (int32_t)freq[i] - target;
    d_q12[i] = d >> 4;
  }

  for (int s = 0; s < 256; ++s) {
    int32_t d = d_q12[s];
    if (d == 0) continue;
    int32_t* row = Wo_.data() + (size_t)s * H_;
    for (int j = 0; j < H_; ++j) {
      int64_t dh = ((int64_t)d * h_[j]) >> fixed::SHIFT;
      int64_t delta = (dh * LR_WO) >> fixed::SHIFT;
      int64_t nv = (int64_t)row[j] - delta;
      int64_t lim = legacy_v1_ ? 32768 : (prof_ == PredProfile::Lite ? 8192 : 32768);
      if (nv > lim) nv = lim;
      if (nv < -lim) nv = -lim;
      row[j] = (int32_t)nv;
    }
    int64_t db = ((int64_t)d * LR_BIAS) >> fixed::SHIFT;
    int64_t nb = (int64_t)bo_[s] - db;
    if (nb > 65536) nb = 65536;
    if (nb < -65536) nb = -65536;
    bo_[s] = (int32_t)nb;
  }

  // Lite: skip recurrent backprop if P(byte) > 90% (58982/65536). v1 keeps the old path.
  bool skip_recurrent = (!legacy_v1_ && prof_ == PredProfile::Lite && freq[true_byte] > 58982);

  int32_t h_old[512];
  int nH = H_;
  for (int i = 0; i < nH; ++i) h_old[i] = h_[i];

  int32_t h_new[512];
  int32_t pre[512];

  if (skip_recurrent) {
    // Only advance hidden state, don't update Wh/Wx/emb/bh
    forward_hidden(h_old, true_byte, h_new, pre);
    for (int i = 0; i < nH; ++i) h_[i] = h_new[i];
    return;
  }

  int32_t dh[512];
  for (int j = 0; j < nH; ++j) {
    int64_t acc = 0;
    for (int s = 0; s < 256; ++s) {
      const int32_t* row = Wo_.data() + (size_t)s * H_;
      acc += (int64_t)d_q12[s] * (int64_t)row[j];
    }
    acc >>= fixed::SHIFT;
    if (acc > 65536) acc = 65536;
    if (acc < -65536) acc = -65536;
    dh[j] = (int32_t)acc;
  }

  forward_hidden(h_old, true_byte, h_new, pre);

  // Multiply by the activation derivative (LUT for Ultra, mask for Lite/v1)
  if (!legacy_v1_ && prof_ == PredProfile::Ultra) {
    for (int j = 0; j < nH; ++j) {
      int32_t der = activate_deriv(pre[j]); // Q12 (0..4096)
      dh[j] = (int32_t)(((int64_t)dh[j] * der) >> fixed::SHIFT);
    }
  } else {
    for (int j = 0; j < nH; ++j) {
      if (pre[j] < -fixed::SCALE || pre[j] > fixed::SCALE) dh[j] = 0;
    }
  }

  int64_t wlim = legacy_v1_ ? 16384 : (prof_ == PredProfile::Lite ? 4096 : 16384);
  int64_t elim = legacy_v1_ ? 8192 : (prof_ == PredProfile::Lite ? 2048 : 8192);
  for (int i = 0; i < nH; ++i) {
    int32_t g = dh[i];
    if (g == 0) continue;
    int32_t* wh_row = Wh_.data() + (size_t)i * H_;
    for (int j = 0; j < nH; ++j) {
      int64_t t = ((int64_t)g * h_old[j]) >> fixed::SHIFT;
      int64_t delta = (t * LR_REC) >> fixed::SHIFT;
      int64_t nv = (int64_t)wh_row[j] - delta;
      if (nv > wlim) nv = wlim;
      if (nv < -wlim) nv = -wlim;
      wh_row[j] = (int32_t)nv;
    }
    int64_t db = ((int64_t)g * LR_REC) >> fixed::SHIFT;
    int64_t nb = (int64_t)bh_[i] - db;
    if (nb > wlim) nb = wlim;
    if (nb < -wlim) nb = -wlim;
    bh_[i] = (int32_t)nb;
  }
  {
    const int32_t* emb_row = emb_.data() + (size_t)true_byte * E_;
    int32_t demb[64];
    for (int k = 0; k < E_; ++k) demb[k] = 0;
    for (int i = 0; i < nH; ++i) {
      int32_t g = dh[i];
      if (g == 0) continue;
      const int32_t* wx_row = Wx_.data() + (size_t)i * E_;
      int32_t* wx_rw = Wx_.data() + (size_t)i * E_;
      for (int k = 0; k < E_; ++k) {
        int64_t t = ((int64_t)g * emb_row[k]) >> fixed::SHIFT;
        int64_t delta = (t * LR_REC) >> fixed::SHIFT;
        int64_t nv = (int64_t)wx_rw[k] - delta;
        if (nv > wlim) nv = wlim;
        if (nv < -wlim) nv = -wlim;
        wx_rw[k] = (int32_t)nv;
        int64_t e = ((int64_t)g * wx_row[k]) >> fixed::SHIFT;
        demb[k] += (int32_t)(((e * LR_REC) >> fixed::SHIFT));
      }
    }
    int32_t* emb_rw = emb_.data() + (size_t)true_byte * E_;
    for (int k = 0; k < E_; ++k) {
      int64_t nv = (int64_t)emb_rw[k] - demb[k];
      if (nv > elim) nv = elim;
      if (nv < -elim) nv = -elim;
      emb_rw[k] = (int32_t)nv;
    }
  }

  forward_hidden(h_old, true_byte, h_new, pre);
  for (int i = 0; i < nH; ++i) h_[i] = h_new[i];
}

} // namespace kzip
