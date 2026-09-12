#include "container.h"
#include "chunk.h"
#include "crc32.h"
#include "predictor.h"
#include "solid.h"
#include "thread_pool.h"
#include "../include/kzip/config.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>

namespace fs = std::filesystem;
namespace kzip { namespace container {

void put_u16le(std::vector<uint8_t>& o, uint16_t v) {
  o.push_back((uint8_t)(v & 0xFF)); o.push_back((uint8_t)(v >> 8));
}
void put_u32le(std::vector<uint8_t>& o, uint32_t v) {
  o.push_back((uint8_t)(v & 0xFF)); o.push_back((uint8_t)((v >> 8) & 0xFF));
  o.push_back((uint8_t)((v >> 16) & 0xFF)); o.push_back((uint8_t)((v >> 24) & 0xFF));
}
void put_u64le(std::vector<uint8_t>& o, uint64_t v) {
  for (int i = 0; i < 8; ++i) o.push_back((uint8_t)((v >> (i * 8)) & 0xFF));
}
uint16_t get_u16le(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
uint32_t get_u32le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
uint64_t get_u64le(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (i * 8);
  return v;
}

void unix_to_dos(uint64_t unix_time, uint16_t& dos_time, uint16_t& dos_date) {
  time_t t = (time_t)unix_time;
  struct tm tmv;
#if defined(_WIN32)
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  int year = tmv.tm_year + 1900;
  if (year < 1980) year = 1980;
  if (year > 2107) year = 2107;
  dos_time = (uint16_t)(((tmv.tm_hour & 31) << 11) | ((tmv.tm_min & 63) << 5) |
                       ((tmv.tm_sec / 2) & 31));
  dos_date = (uint16_t)(((year - 1980) << 9) | (((tmv.tm_mon + 1) & 15) << 5) |
                       (tmv.tm_mday & 31));
}
uint64_t dos_to_unix(uint16_t dos_time, uint16_t dos_date) {
  struct tm tmv{};
  tmv.tm_hour = (dos_time >> 11) & 31;
  tmv.tm_min = (dos_time >> 5) & 63;
  tmv.tm_sec = (dos_time & 31) * 2;
  tmv.tm_mday = dos_date & 31;
  tmv.tm_mon = (((dos_date >> 5) & 15) - 1);
  tmv.tm_year = ((dos_date >> 9) & 127) + 1980 - 1900;
  tmv.tm_isdst = -1;
  time_t t = mktime(&tmv);
  return t < 0 ? 0 : (uint64_t)t;
}

// ---------------- v1 legacy ----------------
std::vector<uint8_t> encode_stream(const uint8_t* data, size_t n, int level,
                                   int threads) {
  std::vector<uint8_t> out;
  out.push_back(kStreamMagicV1[0]); out.push_back(kStreamMagicV1[1]);
  out.push_back(kStreamMagicV1[2]); out.push_back(kStreamMagicV1[3]);
  put_u16le(out, (uint16_t)level);
  put_u16le(out, 0);
  put_u64le(out, (uint64_t)n);
  if (n == 0) { put_u32le(out, 0); return out; }
  size_t nchunks = (n + kChunkSize - 1) / kChunkSize;
  put_u32le(out, (uint32_t)nchunks);

  struct Ch { uint32_t raw=0, comp=0, crc=0; std::vector<uint8_t> payload; };
  std::vector<Ch> chs(nchunks);

  if (threads <= 0) {
    unsigned hc = std::thread::hardware_concurrency();
    threads = hc ? (int)hc : 4;
  }
  if ((size_t)threads > nchunks) threads = (int)nchunks;
  if (threads <= 1) {
    for (size_t i = 0; i < nchunks; ++i) {
      size_t off = i * kChunkSize;
      size_t len = n - off < kChunkSize ? n - off : kChunkSize;
      chs[i].raw = (uint32_t)len;
      chs[i].crc = crc32_compute(data + off, len);
      chs[i].payload = compress_one_chunk(data + off, len, level);
      chs[i].comp = (uint32_t)chs[i].payload.size();
    }
  } else {
    ThreadPool pool((size_t)threads);
    std::vector<std::future<void>> fus;
    for (size_t i = 0; i < nchunks; ++i) {
      fus.push_back(pool.enqueue([&, i] {
        size_t off = i * kChunkSize;
        size_t len = n - off < kChunkSize ? n - off : (size_t)kChunkSize;
        chs[i].raw = (uint32_t)len;
        chs[i].crc = crc32_compute(data + off, len);
        chs[i].payload = compress_one_chunk(data + off, len, level);
        chs[i].comp = (uint32_t)chs[i].payload.size();
      }));
    }
    for (auto& f : fus) f.get();
  }
  for (size_t i = 0; i < nchunks; ++i) {
    put_u32le(out, chs[i].raw);
    put_u32le(out, chs[i].comp);
    put_u32le(out, chs[i].crc);
  }
  for (size_t i = 0; i < nchunks; ++i)
    out.insert(out.end(), chs[i].payload.begin(), chs[i].payload.end());
  return out;
}

bool decode_stream(const uint8_t* data, size_t n, std::vector<uint8_t>& raw_out) {
  raw_out.clear();
  if (n < 4 + 2 + 2 + 8 + 4) return false;
  if (!(data[0]=='K' && data[1]=='Z' && data[2]==0x01 && data[3]==0x00)) return false;
  int level = get_u16le(data + 4);
  uint64_t orig = get_u64le(data + 8);
  uint32_t nchunks = get_u32le(data + 16);
  const uint8_t* p = data + 20;
  size_t rem = n - 20;
  if (nchunks == 0) return orig == 0;
  if (rem < (size_t)nchunks * 12) return false;
  struct H { uint32_t raw, comp, crc; };
  std::vector<H> hs(nchunks);
  for (uint32_t i = 0; i < nchunks; ++i) {
    hs[i].raw = get_u32le(p); hs[i].comp = get_u32le(p+4); hs[i].crc = get_u32le(p+8);
    p += 12; rem -= 12;
  }
  raw_out.resize((size_t)orig);
  size_t woff = 0;
  for (uint32_t i = 0; i < nchunks; ++i) {
    if (rem < hs[i].comp) return false;
    std::vector<uint8_t> chunk_raw;
    if (!decompress_one_chunk(p, hs[i].comp, chunk_raw, hs[i].raw, level))
      return false;
    if (chunk_raw.size() != hs[i].raw) return false;
    if (crc32_compute(chunk_raw.data(), chunk_raw.size()) != hs[i].crc)
      return false;
    memcpy(raw_out.data() + woff, chunk_raw.data(), chunk_raw.size());
    woff += chunk_raw.size();
    p += hs[i].comp; rem -= hs[i].comp;
  }
  return woff == (size_t)orig;
}

// ---------------- v2 ----------------
// Header: [K,Z,02,00][u16 H][u16 E][u8 profile][u8 flags][u32 seed]
//         [u64 raw_size][u32 nchunks]
//         [manifest if present][chunk table][chunk payloads]
// Chunk table: [u32 raw][u32 comp][u32 crc] * nchunks
static constexpr size_t kV2HeaderMin = 4 + 2 + 2 + 1 + 1 + 4 + 8 + 4; // 26

std::vector<uint8_t> encode_stream_v2(
    const uint8_t* data, size_t n, const V2Params& pr,
    const std::vector<std::pair<std::string, std::vector<uint8_t>>>* /*manifest_opt*/,
    int /*threads*/,
    const std::vector<std::pair<uint64_t, uint32_t>>* /*meta_opt*/) {
  // Keep the API simple: the manifest is passed separately via the overload below.
  // This overload is for single files (no manifest).
  std::vector<uint8_t> out;
  out.push_back('K'); out.push_back('Z'); out.push_back(0x02); out.push_back(0x00);
  put_u16le(out, (uint16_t)pr.H);
  put_u16le(out, (uint16_t)pr.E);
  out.push_back(pr.profile);
  out.push_back(0x00); // flags: single
  put_u32le(out, pr.seed);
  put_u64le(out, (uint64_t)n);
  if (n == 0) { put_u32le(out, 0); return out; }
  size_t nchunks = (n + kChunkSize - 1) / kChunkSize;
  put_u32le(out, (uint32_t)nchunks);
  // Compress sequentially, continuous predictor (solid)
  MicroPredictor pred(pr.H, pr.E, pr.seed);
  pred.set_profile(pr.profile == 2 ? PredProfile::Ultra : PredProfile::Lite);
  struct Ch { uint32_t raw, comp, crc; std::vector<uint8_t> p; };
  std::vector<Ch> chs;
  chs.reserve(nchunks);
  for (size_t i = 0; i < nchunks; ++i) {
    size_t off = i * kChunkSize, len = n - off < kChunkSize ? n - off : kChunkSize;
    Ch c;
    c.raw = (uint32_t)len;
    c.crc = crc32_compute(data + off, len);
    c.p = compress_chunk_with_pred(data + off, len, pred);
    c.comp = (uint32_t)c.p.size();
    chs.push_back(std::move(c));
  }
  for (auto& c : chs) { put_u32le(out, c.raw); put_u32le(out, c.comp); put_u32le(out, c.crc); }
  for (auto& c : chs) out.insert(out.end(), c.p.begin(), c.p.end());
  return out;
}

static std::vector<uint8_t> encode_solid_with_manifest(
    const uint8_t* data, size_t n, const V2Params& pr,
    const std::vector<solid::ManifestEntry>& man) {
  std::vector<uint8_t> out;
  out.push_back('K'); out.push_back('Z'); out.push_back(0x02); out.push_back(0x00);
  put_u16le(out, (uint16_t)pr.H);
  put_u16le(out, (uint16_t)pr.E);
  out.push_back(pr.profile);
  out.push_back(kFlagManifest);
  put_u32le(out, pr.seed);
  put_u64le(out, (uint64_t)n);
  // Binary manifest (uncompressed, right after the header for fast `l` reads)
  std::vector<uint8_t> mbuf;
  solid::encode_manifest(man, mbuf);
  put_u32le(out, (uint32_t)mbuf.size());
  out.insert(out.end(), mbuf.begin(), mbuf.end());
  if (n == 0) { put_u32le(out, 0); return out; }
  size_t nchunks = (n + kChunkSize - 1) / kChunkSize;
  put_u32le(out, (uint32_t)nchunks);
  MicroPredictor pred(pr.H, pr.E, pr.seed);
  pred.set_profile(pr.profile == 2 ? PredProfile::Ultra : PredProfile::Lite);
  struct Ch { uint32_t raw, comp, crc; std::vector<uint8_t> p; };
  std::vector<Ch> chs;
  chs.reserve(nchunks);
  for (size_t i = 0; i < nchunks; ++i) {
    size_t off = i * kChunkSize, len = n - off < kChunkSize ? n - off : kChunkSize;
    Ch c;
    c.raw = (uint32_t)len;
    c.crc = crc32_compute(data + off, len);
    c.p = compress_chunk_with_pred(data + off, len, pred);
    c.comp = (uint32_t)c.p.size();
    chs.push_back(std::move(c));
  }
  for (auto& c : chs) { put_u32le(out, c.raw); put_u32le(out, c.comp); put_u32le(out, c.crc); }
  for (auto& c : chs) out.insert(out.end(), c.p.begin(), c.p.end());
  return out;
}

// Public wrapper for solid (declared in container.h, overloaded in api.cpp)
std::vector<uint8_t> encode_stream_v2_solid(const uint8_t* data, size_t n,
                                            const V2Params& pr,
                                            const std::vector<solid::ManifestEntry>& man) {
  return encode_solid_with_manifest(data, n, pr, man);
}

static bool decode_v2_common(const uint8_t* data, size_t n,
                             std::vector<uint8_t>& raw_out, V2Params* pr_out,
                             std::vector<solid::ManifestEntry>* man_out) {
  raw_out.clear();
  if (n < kV2HeaderMin) return false;
  if (!(data[0]=='K' && data[1]=='Z' && data[2]==0x02 && data[3]==0x00)) return false;
  V2Params pr;
  pr.H = get_u16le(data + 4);
  pr.E = get_u16le(data + 6);
  pr.profile = data[8];
  uint8_t flags = data[9];
  pr.seed = get_u32le(data + 10);
  uint64_t raw_size = get_u64le(data + 14);
  const uint8_t* p = data + 22;
  size_t rem = n - 22;
  std::vector<solid::ManifestEntry> man;
  if (flags & kFlagManifest) {
    if (rem < 4) return false;
    uint32_t mlen = get_u32le(p); p += 4; rem -= 4;
    if (rem < mlen) return false;
    size_t consumed = 0;
    if (!solid::decode_manifest(p, mlen, man, consumed)) return false;
    if (consumed != mlen) return false;
    p += mlen; rem -= mlen;
  }
  if (rem < 4) return false;
  uint32_t nchunks = get_u32le(p); p += 4; rem -= 4;
  if (nchunks == 0) {
    if (raw_size != 0) return false;
    if (pr_out) *pr_out = pr;
    if (man_out) *man_out = man;
    return true;
  }
  if (rem < (size_t)nchunks * 12) return false;
  struct H { uint32_t raw, comp, crc; };
  std::vector<H> hs(nchunks);
  for (uint32_t i = 0; i < nchunks; ++i) {
    hs[i].raw = get_u32le(p); hs[i].comp = get_u32le(p+4); hs[i].crc = get_u32le(p+8);
    p += 12; rem -= 12;
  }
  if (pr.H < 8 || pr.H > 512 || pr.E < 4 || pr.E > 64) return false;
  MicroPredictor pred(pr.H, pr.E, pr.seed);
  pred.set_profile(pr.profile == 2 ? PredProfile::Ultra : PredProfile::Lite);
  raw_out.resize((size_t)raw_size);
  size_t woff = 0;
  for (uint32_t i = 0; i < nchunks; ++i) {
    if (rem < hs[i].comp) return false;
    std::vector<uint8_t> cr;
    if (!decompress_chunk_with_pred(p, hs[i].comp, nullptr, hs[i].raw, pred, cr))
      return false;
    if (cr.size() != hs[i].raw) return false;
    if (!cr.empty() && crc32_compute(cr.data(), cr.size()) != hs[i].crc) return false;
    if (woff + cr.size() > raw_out.size()) return false;
    memcpy(raw_out.data() + woff, cr.data(), cr.size());
    woff += cr.size();
    p += hs[i].comp; rem -= hs[i].comp;
  }
  if (woff != (size_t)raw_size) return false;
  if (pr_out) *pr_out = pr;
  if (man_out) *man_out = man;
  return true;
}

bool decode_stream_v2(const uint8_t* data, size_t n,
                      std::vector<uint8_t>& raw_out, V2Params* pr_out,
                      std::vector<std::string>* paths_out,
                      std::vector<uint64_t>* sizes_out,
                      std::vector<uint64_t>* mtimes_out,
                      std::vector<uint32_t>* modes_out,
                      std::vector<uint32_t>* crcs_out) {
  std::vector<solid::ManifestEntry> man;
  V2Params pr;
  if (!decode_v2_common(data, n, raw_out, &pr, &man)) return false;
  if (pr_out) *pr_out = pr;
  if (paths_out) {
    paths_out->clear(); sizes_out->clear(); mtimes_out->clear();
    modes_out->clear(); crcs_out->clear();
    for (auto& e : man) {
      paths_out->push_back(e.path);
      sizes_out->push_back(e.size);
      mtimes_out->push_back(e.mtime);
      modes_out->push_back(e.mode);
      crcs_out->push_back(e.crc);
    }
  }
  return true;
}

bool peek_stream_v2(const uint8_t* data, size_t n, V2Params& pr,
                    uint64_t& raw_size, uint32_t& nchunks, bool& has_manifest,
                    std::vector<std::string>& paths, std::vector<uint64_t>& sizes) {
  if (n < kV2HeaderMin) return false;
  if (!(data[0]=='K' && data[1]=='Z' && data[2]==0x02 && data[3]==0x00)) return false;
  pr.H = get_u16le(data + 4);
  pr.E = get_u16le(data + 6);
  pr.profile = data[8];
  uint8_t flags = data[9];
  pr.seed = get_u32le(data + 10);
  raw_size = get_u64le(data + 14);
  has_manifest = (flags & kFlagManifest) != 0;
  const uint8_t* p = data + 22;
  size_t rem = n - 22;
  paths.clear(); sizes.clear();
  if (has_manifest) {
    if (rem < 4) return false;
    uint32_t mlen = get_u32le(p); p += 4; rem -= 4;
    if (rem < mlen) return false;
    std::vector<solid::ManifestEntry> man;
    size_t consumed = 0;
    if (!solid::decode_manifest(p, mlen, man, consumed)) return false;
    for (auto& e : man) { paths.push_back(e.path); sizes.push_back(e.size); }
    p += mlen; rem -= mlen;
  }
  if (rem < 4) return false;
  nchunks = get_u32le(p);
  return true;
}

// ---------------- ZIP IO (shared v1/v2) ----------------
// NOTE: write offsets are tracked with an explicit uint64_t counter instead of
// ftell(), whose long return type truncates past 2GB on Windows (LLP64).
bool write_archive(const std::string& path, std::vector<FileItem>& items) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  uint64_t pos = 0;
  bool io_err = false;
  auto put = [&](const void* p, size_t n) {
    if (io_err || n == 0 || p == nullptr) return;
    if (fwrite(p, 1, n, f) != n) io_err = true;
    else pos += (uint64_t)n;
  };
  auto put_vec = [&](const std::vector<uint8_t>& v) {
    if (!v.empty()) put(v.data(), v.size());
  };
  auto put_str = [&](const std::string& s) {
    if (!s.empty()) put(s.data(), s.size());
  };
  struct CD { std::string name; uint16_t method; uint16_t t,d; uint32_t crc;
              uint64_t comp, uncomp; uint64_t lho; uint32_t ext; bool dir; };
  std::vector<CD> cds;
  for (auto& it : items) {
    uint64_t lho = pos;
    uint16_t dos_t, dos_d;
    unix_to_dos(it.mtime, dos_t, dos_d);
    uint16_t method = it.is_dir ? 0 : it.method;
    if (!it.is_dir && method != 0 && method != kMethodId) method = kMethodId;
    uint32_t crc = it.crc;
    uint64_t comp = it.comp_data.size();
    uint64_t uncomp = it.size;
    if (it.is_dir) { comp = 0; uncomp = 0; crc = 0; method = 0; }
    if (method == 0) { comp = uncomp; } // STORE: comp_data is raw (or empty)
    uint8_t lh[30];
    lh[0]='P';lh[1]='K';lh[2]=3;lh[3]=4;
    lh[4]=20;lh[5]=0;
    lh[6]=0x00;lh[7]=0x08;
    lh[8]=method&0xFF;lh[9]=(method>>8)&0xFF;
    lh[10]=dos_t&0xFF;lh[11]=(dos_t>>8)&0xFF;
    lh[12]=dos_d&0xFF;lh[13]=(dos_d>>8)&0xFF;
    lh[14]=crc&0xFF;lh[15]=(crc>>8)&0xFF;lh[16]=(crc>>16)&0xFF;lh[17]=(crc>>24)&0xFF;
    uint32_t c32 = comp > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)comp;
    uint32_t u32 = uncomp > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)uncomp;
    lh[18]=c32&0xFF;lh[19]=(c32>>8)&0xFF;lh[20]=(c32>>16)&0xFF;lh[21]=(c32>>24)&0xFF;
    lh[22]=u32&0xFF;lh[23]=(u32>>8)&0xFF;lh[24]=(u32>>16)&0xFF;lh[25]=(u32>>24)&0xFF;
    uint16_t nl=(uint16_t)it.arcname.size();
    lh[26]=nl&0xFF;lh[27]=(nl>>8)&0xFF;
    lh[28]=0;lh[29]=0;
    put(lh,30);
    put_str(it.arcname);
    if (!it.is_dir) {
      if (method == kMethodId) {
        put_vec(it.comp_data);
      } else {
        // STORE: comp_data holds raw (preloaded); empty with size>0 means caller error
        put_vec(it.comp_data);
      }
    }
    uint32_t ext = (it.mode << 16);
    if (it.is_dir) ext |= 0x10;
    else ext |= 0x20;
    cds.push_back({it.arcname, method, dos_t, dos_d, crc, comp, uncomp, lho, ext, it.is_dir});
  }
  uint64_t cd_off = pos;
  uint64_t cd_size = 0;
  for (auto& c : cds) {
    uint64_t start = pos;
    uint8_t ch[46];
    ch[0]='P';ch[1]='K';ch[2]=1;ch[3]=2;
    ch[4]=63;ch[5]=0;
    ch[6]=20;ch[7]=0;
    ch[8]=0x00;ch[9]=0x08;
    ch[10]=c.method&0xFF;ch[11]=(c.method>>8)&0xFF;
    ch[12]=c.t&0xFF;ch[13]=(c.t>>8)&0xFF;
    ch[14]=c.d&0xFF;ch[15]=(c.d>>8)&0xFF;
    ch[16]=c.crc&0xFF;ch[17]=(c.crc>>8)&0xFF;ch[18]=(c.crc>>16)&0xFF;ch[19]=(c.crc>>24)&0xFF;
    uint32_t cc=(uint32_t)(c.comp>0xFFFFFFFF?0xFFFFFFFF:c.comp);
    uint32_t uu=(uint32_t)(c.uncomp>0xFFFFFFFF?0xFFFFFFFF:c.uncomp);
    ch[20]=cc&0xFF;ch[21]=(cc>>8)&0xFF;ch[22]=(cc>>16)&0xFF;ch[23]=(cc>>24)&0xFF;
    ch[24]=uu&0xFF;ch[25]=(uu>>8)&0xFF;ch[26]=(uu>>16)&0xFF;ch[27]=(uu>>24)&0xFF;
    uint16_t nl=(uint16_t)c.name.size();
    ch[28]=nl&0xFF;ch[29]=(nl>>8)&0xFF;
    ch[30]=0;ch[31]=0;ch[32]=0;ch[33]=0;
    ch[34]=0;ch[35]=0;
    ch[36]=0;ch[37]=0;
    ch[38]=c.ext&0xFF;ch[39]=(c.ext>>8)&0xFF;ch[40]=(c.ext>>16)&0xFF;ch[41]=(c.ext>>24)&0xFF;
    uint32_t lo=(uint32_t)(c.lho>0xFFFFFFFF?0xFFFFFFFF:c.lho);
    ch[42]=lo&0xFF;ch[43]=(lo>>8)&0xFF;ch[44]=(lo>>16)&0xFF;ch[45]=(lo>>24)&0xFF;
    put(ch,46);
    put_str(c.name);
    cd_size += pos - start;
  }
  uint8_t eocd[22];
  eocd[0]='P';eocd[1]='K';eocd[2]=5;eocd[3]=6;
  eocd[4]=0;eocd[5]=0;eocd[6]=0;eocd[7]=0;
  uint16_t nn=(uint16_t)cds.size();
  eocd[8]=nn&0xFF;eocd[9]=(nn>>8)&0xFF;
  eocd[10]=nn&0xFF;eocd[11]=(nn>>8)&0xFF;
  uint32_t csz=(uint32_t)(cd_size>0xFFFFFFFF?0xFFFFFFFF:cd_size);
  uint32_t coff=(uint32_t)(cd_off>0xFFFFFFFF?0xFFFFFFFF:cd_off);
  eocd[12]=csz&0xFF;eocd[13]=(csz>>8)&0xFF;eocd[14]=(csz>>16)&0xFF;eocd[15]=(csz>>24)&0xFF;
  eocd[16]=coff&0xFF;eocd[17]=(coff>>8)&0xFF;eocd[18]=(coff>>16)&0xFF;eocd[19]=(coff>>24)&0xFF;
  eocd[20]=0;eocd[21]=0;
  put(eocd,22);
  if (fclose(f) != 0) io_err = true;
  return !io_err;
}

bool load_archive_file(const std::string& path, std::vector<uint8_t>& out) {
  out.clear();
  std::error_code ec;
  uintmax_t n = fs::file_size(path, ec);
  if (ec) return false;
  if (n > (uintmax_t)SIZE_MAX) return false; // not addressable in RAM
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.resize((size_t)n);
  if (n > 0 && !f.read((char*)out.data(), (std::streamsize)n)) return false;
  return true;
}

bool read_central_from_buf(const std::vector<uint8_t>& buf,
                           std::vector<ArchiveEntry>& entries,
                           std::vector<uint64_t>& lho_offsets,
                           std::vector<uint64_t>& comp_sizes,
                           std::vector<uint64_t>& uncomp_sizes,
                           std::vector<uint32_t>& crcs,
                           std::vector<uint16_t>& methods) {
  if (buf.size() < 22) return false;
  size_t eocd_pos = SIZE_MAX;
  size_t start = buf.size() > 65557 + 22 ? buf.size() - (65557 + 22) : 0;
  for (size_t i = buf.size() - 22; ; --i) {
    if (buf[i]=='P'&&buf[i+1]=='K'&&buf[i+2]==5&&buf[i+3]==6){eocd_pos=i;break;}
    if (i==start) break;
  }
  if (eocd_pos==SIZE_MAX) return false;
  const uint8_t* e = buf.data()+eocd_pos;
  uint16_t total = get_u16le(e+10);
  uint32_t cd_size = get_u32le(e+12);
  uint32_t cd_off = get_u32le(e+16);
  (void)cd_size;
  if ((size_t)cd_off > buf.size()) return false;
  size_t p = cd_off;
  for (int i = 0; i < total; ++i) {
    if (p + 46 > buf.size()) return false;
    const uint8_t* c = buf.data()+p;
    if (!(c[0]=='P'&&c[1]=='K'&&c[2]==1&&c[3]==2)) return false;
    uint16_t method=get_u16le(c+10);
    uint16_t t=get_u16le(c+12), d=get_u16le(c+14);
    uint32_t crc=get_u32le(c+16);
    uint32_t cs=get_u32le(c+20), us=get_u32le(c+24);
    uint16_t nl=get_u16le(c+28), el=get_u16le(c+30), cl=get_u16le(c+32);
    uint32_t ext=get_u32le(c+38);
    uint32_t lo=get_u32le(c+42);
    p+=46;
    if (p+nl+el+cl > buf.size()) return false;
    std::string name((char*)buf.data()+p, nl);
    p+=nl+el+cl;
    ArchiveEntry en;
    en.name=name; en.comp_size=cs; en.uncomp_size=us; en.crc32=crc; en.method=method;
    en.is_dir = (!name.empty()&&name.back()=='/') || ((ext&0x10)!=0 && cs==0 && us==0);
    en.mtime = dos_to_unix(t, d);
    en.mode = (ext >> 16);
    if (en.mode == 0) en.mode = en.is_dir ? (0040000|0755) : (0100000|0644);
    entries.push_back(en);
    lho_offsets.push_back(lo);
    comp_sizes.push_back(cs);
    uncomp_sizes.push_back(us);
    crcs.push_back(crc);
    methods.push_back(method);
  }
  return true;
}

bool read_central(const std::string& path, std::vector<ArchiveEntry>& entries,
                  std::vector<uint64_t>& lho_offsets,
                  std::vector<uint64_t>& comp_sizes,
                  std::vector<uint64_t>& uncomp_sizes,
                  std::vector<uint32_t>& crcs,
                  std::vector<uint16_t>& methods) {
  std::vector<uint8_t> buf;
  if (!load_archive_file(path, buf)) return false;
  return read_central_from_buf(buf, entries, lho_offsets, comp_sizes,
                               uncomp_sizes, crcs, methods);
}

bool read_entry_payload_from_buf(const std::vector<uint8_t>& buf, uint64_t lho_offset,
                                 std::vector<uint8_t>& payload,
                                 uint64_t& uncomp_size, uint32_t& crc, uint16_t& method,
                                 std::string& arcname) {
  if (lho_offset + 30 > buf.size()) return false;
  const uint8_t* h = buf.data()+lho_offset;
  if (!(h[0]=='P'&&h[1]=='K'&&h[2]==3&&h[3]==4)) return false;
  method=get_u16le(h+8);
  crc=get_u32le(h+14);
  uint32_t cs=get_u32le(h+18), us=get_u32le(h+22);
  uint16_t nl=get_u16le(h+26), el=get_u16le(h+28);
  if (lho_offset+30+nl+el+cs > buf.size()) return false;
  arcname.assign((char*)h+30, nl);
  uncomp_size=us;
  payload.assign(h+30+nl+el, h+30+nl+el+cs);
  return true;
}

bool read_entry_payload(const std::string& path, uint64_t lho_offset,
                        std::vector<uint8_t>& payload,
                        uint64_t& uncomp_size, uint32_t& crc, uint16_t& method,
                        std::string& arcname) {
  std::vector<uint8_t> buf;
  if (!load_archive_file(path, buf)) return false;
  return read_entry_payload_from_buf(buf, lho_offset, payload, uncomp_size,
                                     crc, method, arcname);
}

}} // namespace kzip::container
