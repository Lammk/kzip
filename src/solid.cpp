#include "solid.h"
#include "crc32.h"
#include "prefilter.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>

namespace fs = std::filesystem;
namespace kzip { namespace solid {

static std::string to_posix(const fs::path& p) {
  return p.generic_string(); // already uses '/'
}

static std::string lower_ext(const std::string& arcname) {
  size_t slash = arcname.find_last_of('/');
  size_t dot = arcname.find_last_of('.');
  if (dot == std::string::npos) return "";
  if (slash != std::string::npos && dot < slash) return "";
  std::string e = arcname.substr(dot);
  for (auto& c : e) c = (char)tolower((unsigned char)c);
  return e;
}

bool scan_inputs(const std::vector<std::string>& inputs, ScannedTree& out) {
  out.files.clear();
  out.dirs.clear();
  for (auto& in : inputs) {
    fs::path ip(in);
    std::error_code ec;
    ec.clear();
    bool ex = fs::exists(ip, ec);
    if (ec) ec.clear();
    if (!ex) return false;
    ec.clear();
    if (fs::is_directory(ip, ec)) {
      ec.clear();
      fs::path parent = ip.parent_path();
      auto opts = fs::directory_options::skip_permission_denied;
      for (auto it = fs::recursive_directory_iterator(ip, opts, ec);
           it != fs::recursive_directory_iterator(); ) {
        if (ec) { ec.clear(); it.increment(ec); if (ec) { ec.clear(); break; } continue; }
        std::error_code ec2;
        fs::path rel = fs::relative(it->path(), parent, ec2);
        if (ec2) { it.increment(ec); continue; }
        ScannedFile sf;
        sf.arcname = to_posix(rel);
        sf.fspath = it->path().string();
        ec2.clear();
        sf.is_dir = it->is_directory(ec2);
        if (ec2) {
          // fallback via stat when d_type is unknown (overlayfs)
          struct stat st0{};
          sf.is_dir = (::stat(it->path().c_str(), &st0) == 0) &&
                      S_ISDIR(st0.st_mode);
        }
        struct stat st{};
        if (::stat(it->path().c_str(), &st) == 0) {
          sf.mtime = (uint64_t)st.st_mtime;
          sf.mode = (uint32_t)st.st_mode;
          sf.size = sf.is_dir ? 0 : (uint64_t)st.st_size;
        } else {
          sf.mtime = 0;
          sf.mode = sf.is_dir ? (0040000 | 0755) : (0100000 | 0644);
        }
        if (sf.is_dir) {
          if (!sf.arcname.empty() && sf.arcname.back() != '/') sf.arcname += '/';
          sf.ext = "";
          out.dirs.push_back(std::move(sf));
        } else {
          sf.ext = lower_ext(sf.arcname);
          sf.incompressible_hint = prefilter::extension_likely_compressed(sf.ext);
          out.files.push_back(std::move(sf));
        }
        it.increment(ec);
      }
    } else {
      ScannedFile sf;
      sf.arcname = to_posix(ip.filename());
      sf.fspath = ip.string();
      sf.is_dir = false;
      struct stat st{};
      if (::stat(ip.c_str(), &st) == 0) {
        sf.mtime = (uint64_t)st.st_mtime;
        sf.mode = (uint32_t)st.st_mode;
        sf.size = (uint64_t)st.st_size;
      }
      sf.ext = lower_ext(sf.arcname);
      sf.incompressible_hint = prefilter::extension_likely_compressed(sf.ext);
      out.files.push_back(std::move(sf));
    }
  }
  return true;
}

void sort_smart(std::vector<ScannedFile>& files) {
  std::sort(files.begin(), files.end(), [](const ScannedFile& a, const ScannedFile& b) {
    if (a.ext != b.ext) return a.ext < b.ext;
    auto pa = a.arcname.rfind('/');
    auto pb = b.arcname.rfind('/');
    std::string da = pa == std::string::npos ? "" : a.arcname.substr(0, pa);
    std::string db = pb == std::string::npos ? "" : b.arcname.substr(0, pb);
    if (da != db) return da < db;
    if (a.size != b.size) return a.size < b.size;
    return a.arcname < b.arcname;
  });
}

std::vector<SolidGroup> pack_groups(const std::vector<ScannedFile>& files,
                                    uint64_t target, uint64_t maxb) {
  std::vector<SolidGroup> gs;
  SolidGroup cur;
  for (size_t i = 0; i < files.size(); ++i) {
    uint64_t sz = files[i].size;
    if (cur.idx.empty()) {
      cur.idx.push_back(i);
      cur.raw_size = sz;
      if (cur.raw_size >= maxb) { gs.push_back(cur); cur = SolidGroup(); }
      continue;
    }
    if (cur.raw_size + sz > maxb) {
      gs.push_back(cur);
      cur = SolidGroup();
      cur.idx.push_back(i);
      cur.raw_size = sz;
      if (cur.raw_size >= maxb) { gs.push_back(cur); cur = SolidGroup(); }
    } else {
      cur.idx.push_back(i);
      cur.raw_size += sz;
      if (cur.raw_size >= target) {
        // Split once target is reached (growth up to max still allowed next round)
        // To avoid too many tiny groups, only split if the next file differs in ext?
        // Simple path: split as soon as >= target.
        gs.push_back(cur);
        cur = SolidGroup();
      }
    }
  }
  if (!cur.idx.empty()) gs.push_back(cur);
  return gs;
}

static void w16(std::vector<uint8_t>& o, uint16_t v) {
  o.push_back(v & 0xFF); o.push_back(v >> 8);
}
static void w32(std::vector<uint8_t>& o, uint32_t v) {
  for (int i = 0; i < 4; ++i) o.push_back((v >> (i * 8)) & 0xFF);
}
static void w64(std::vector<uint8_t>& o, uint64_t v) {
  for (int i = 0; i < 8; ++i) o.push_back((v >> (i * 8)) & 0xFF);
}
static uint16_t r16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static uint32_t r32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t r64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (i * 8);
  return v;
}

void encode_manifest(const std::vector<ManifestEntry>& m, std::vector<uint8_t>& out) {
  w32(out, (uint32_t)m.size());
  for (auto& e : m) {
    w16(out, (uint16_t)e.path.size());
    out.insert(out.end(), e.path.begin(), e.path.end());
    w64(out, e.size);
    w64(out, e.mtime);
    w32(out, e.mode);
    w32(out, e.crc);
  }
}

bool decode_manifest(const uint8_t* data, size_t n, std::vector<ManifestEntry>& m,
                     size_t& consumed) {
  m.clear();
  if (n < 4) return false;
  uint32_t nf = r32(data);
  size_t p = 4;
  if (nf > 200000) return false;
  for (uint32_t i = 0; i < nf; ++i) {
    if (p + 2 > n) return false;
    uint16_t L = r16(data + p); p += 2;
    if (p + L + 8 + 8 + 4 + 4 > n) return false;
    ManifestEntry e;
    e.path.assign((const char*)data + p, L); p += L;
    // Block path traversal at a higher decode layer; only parse here
    e.size = r64(data + p); p += 8;
    e.mtime = r64(data + p); p += 8;
    e.mode = r32(data + p); p += 4;
    e.crc = r32(data + p); p += 4;
    if (e.path.empty() || e.path.size() > 4096) return false;
    m.push_back(std::move(e));
  }
  consumed = p;
  return true;
}

bool read_file_bytes(const std::string& p, std::vector<uint8_t>& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return true;
}

uint32_t file_crc32(const std::string& p, uint64_t* size_out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return 0;
  uint32_t crc = 0xFFFFFFFFu;
  // use a private table to avoid lazy-init coupling? compute in chunks via helper
  char buf[65536];
  uint64_t total = 0;
  // compute manually to stream large files
  static uint32_t T[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      T[i] = c;
    }
    init = true;
  }
  while (f) {
    f.read(buf, sizeof(buf));
    std::streamsize g = f.gcount();
    for (std::streamsize i = 0; i < g; ++i)
      crc = T[(crc ^ (uint8_t)buf[i]) & 0xFF] ^ (crc >> 8);
    total += (uint64_t)g;
  }
  if (size_out) *size_out = total;
  return crc ^ 0xFFFFFFFFu;
}

}} // namespace kzip::solid
