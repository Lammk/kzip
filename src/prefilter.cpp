#include "prefilter.h"
#include "../include/kzip/config.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace kzip { namespace prefilter {

const char* match_magic(const uint8_t* d, size_t n) {
  if (!d || n < 4) return nullptr;
  // JPEG
  if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return "jpeg";
  // PNG
  if (n >= 8 && d[0]==0x89 && d[1]==0x50 && d[2]==0x4E && d[3]==0x47 &&
      d[4]==0x0D && d[5]==0x0A && d[6]==0x1A && d[7]==0x0A) return "png";
  // GIF
  if (n >= 6 && d[0]=='G' && d[1]=='I' && d[2]=='F' && d[3]=='8') return "gif";
  // WEBP: RIFF....WEBP
  if (n >= 12 && d[0]=='R' && d[1]=='I' && d[2]=='F' && d[3]=='F' &&
      d[8]=='W' && d[9]=='E' && d[10]=='B' && d[11]=='P') return "webp";
  // MP4/MOV/HEIF: ftyp at offset 4
  if (n >= 12 && d[4]=='f' && d[5]=='t' && d[6]=='y' && d[7]=='p') return "mp4";
  // ZIP / JAR / DOCX...: PK\x03\x04 (local), PK\x01\x02 (central), PK\x05\x06 (eocd)
  if (d[0]=='P' && d[1]=='K' &&
      ((d[2]==0x03 && d[3]==0x04) || (d[2]==0x01 && d[3]==0x02) ||
       (d[2]==0x05 && d[3]==0x06) || (d[2]==0x07 && d[3]==0x08)))
    return "zip";
  // 7z
  if (n >= 6 && d[0]==0x37 && d[1]==0x7A && d[2]==0xBC && d[3]==0xAF &&
      d[4]==0x27 && d[5]==0x1C) return "7z";
  // GZ
  if (d[0]==0x1F && d[1]==0x8B) return "gzip";
  // Zstd (LE magic 28 B5 2F FD)
  if (d[0]==0x28 && d[1]==0xB5 && d[2]==0x2F && d[3]==0xFD) return "zstd";
  // RAR
  if (n >= 7 && d[0]==0x52 && d[1]==0x61 && d[2]==0x72 && d[3]==0x21 &&
      d[4]==0x1A && d[5]==0x07) return "rar";
  // BZ2
  if (d[0]=='B' && d[1]=='Z' && d[2]=='h') return "bzip2";
  // XZ
  if (n >= 6 && d[0]==0xFD && d[1]==0x37 && d[2]==0x7A && d[3]==0x58 &&
      d[4]==0x5A && d[5]==0x00) return "xz";
  // OGG
  if (d[0]=='O' && d[1]=='g' && d[2]=='g' && d[3]=='S') return "ogg";
  // MP3: ID3 or frame sync
  if ((d[0]=='I' && d[1]=='D' && d[2]=='3') ||
      (d[0]==0xFF && (d[1] & 0xE0) == 0xE0)) return "mp3";
  // WOFF2 (compressed font): wOF2
  if (d[0]=='w' && d[1]=='O' && d[2]=='F' && d[3]=='2') return "woff2";
  return nullptr;
}

// LUT: clog2c[c] = c*log2(c) (double), c=0..65536. Init once (may use
// libm at init, hot path is lookup only). log2lut[n] = log2(n).
static std::once_flag g_lut_once;
static std::vector<double> g_clog2c;
static std::vector<double> g_log2n;
static void init_lut() {
  g_clog2c.resize(65537);
  g_clog2c[0] = 0.0;
  for (size_t c = 1; c <= 65536; ++c)
    g_clog2c[c] = (double)c * (std::log((double)c) / std::log(2.0));
  g_log2n.resize(65537);
  g_log2n[0] = 0.0;
  for (size_t c = 1; c <= 65536; ++c)
    g_log2n[c] = std::log((double)c) / std::log(2.0);
}

double shannon_entropy_sample(const uint8_t* data, size_t n, size_t* sampled_out) {
  std::call_once(g_lut_once, init_lut);
  size_t m = n < kEntropySample ? n : kEntropySample;
  if (sampled_out) *sampled_out = m;
  if (m == 0) return 0.0;
  size_t hist[256] = {0};
  for (size_t i = 0; i < m; ++i) hist[data[i]]++;
  // H = log2(m) - sum(c*log2(c))/m  (for c>0)
  double s = 0.0;
  for (int i = 0; i < 256; ++i) {
    size_t c = hist[i];
    if (c) s += g_clog2c[c];
  }
  double h = g_log2n[m] - s / (double)m;
  if (h < 0 && h > -1e-9) h = 0;
  return h;
}

Verdict analyze(const uint8_t* data, size_t n) {
  Verdict v;
  if (n == 0) { v.reason = "empty"; v.should_store = true; return v; }
  if (const char* t = match_magic(data, n)) {
    // Save the type into a static buffer so reason points to stable storage
    thread_local char buf[32];
    snprintf(buf, sizeof(buf), "magic:%.20s", t);
    v.should_store = true;
    v.reason = buf;
    size_t s = 0;
    v.entropy = shannon_entropy_sample(data, n, &s);
    v.sampled = s;
    return v;
  }
  size_t s = 0;
  double h = shannon_entropy_sample(data, n, &s);
  v.entropy = h;
  v.sampled = s;
  if (h >= kEntropyThreshold) {
    v.should_store = true;
    v.reason = "entropy>=7.92";
  }
  return v;
}

bool extension_likely_compressed(const std::string& path_or_ext) {
  std::string e;
  size_t dot = path_or_ext.find_last_of('.');
  size_t slash = path_or_ext.find_last_of("/\\");
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
    e = path_or_ext.substr(dot);
  else
    e = path_or_ext;
  for (auto& c : e) c = (char)tolower((unsigned char)c);
  static const char* kExt[] = {".jpg",".jpeg",".png",".gif",".webp",".mp4",".mov",
    ".mkv",".avi",".mp3",".ogg",".zip",".7z",".gz",".tgz",".zst",".rar",".bz2",
    ".xz",".woff2",".pdf",".exe",".dmg",".iso",".parquet",".arrow",nullptr};
  for (int i = 0; kExt[i]; ++i)
    if (e == kExt[i]) return true;
  return false;
}

}} // namespace kzip::prefilter
