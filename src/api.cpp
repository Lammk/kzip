#include "../include/kzip/kzip.h"
#include "container.h"
#include "chunk.h"
#include "crc32.h"
#include "sha256.h"
#include "predictor.h"
#include "prefilter.h"
#include "profile.h"
#include "solid.h"
#include "thread_pool.h"
#include "../include/kzip/config.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <thread>
#include <sys/stat.h>
#if !defined(_WIN32)
#include <fcntl.h>
#include <utime.h>
#endif

namespace fs = std::filesystem;

namespace kzip {

int default_threads() {
  unsigned hc = std::thread::hardware_concurrency();
  return hc ? (int)hc : 4;
}

int param_count_for_level(int level) {
  int H, E;
  level_to_hidden_embed(level, H, E);
  return param_count_for_hidden_embed(H, E);
}
int param_count_for_profile(int pid) {
  Profile p = pid == 2 ? Profile::Ultra : Profile::Lite;
  int H, E;
  profile_hidden_embed(p, H, E);
  return param_count_for_hidden_embed(H, E);
}
const char* profile_name_of(int pid) {
  return pid == 2 ? "ultra" : (pid == 1 ? "lite" : "auto");
}

static void put_u32le(std::vector<uint8_t>& o, uint32_t v) {
  o.push_back((uint8_t)(v & 0xFF)); o.push_back((uint8_t)((v >> 8) & 0xFF));
  o.push_back((uint8_t)((v >> 16) & 0xFF)); o.push_back((uint8_t)((v >> 24) & 0xFF));
}
static uint32_t get_u32le(const uint8_t* p) {
  return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}

// ---------------- Buffer v1 (frozen) ----------------
bool compress_buffer(const uint8_t* data, size_t size,
                     std::vector<uint8_t>& out, int level, int threads,
                     ProgressCb progress) {
  out.clear();
  if (level < 1) level = 1;
  if (level > 9) level = 9;
  out.push_back('K'); out.push_back('Z'); out.push_back('B'); out.push_back(0x01);
  out.push_back((uint8_t)(level & 0xFF)); out.push_back((uint8_t)(level >> 8));
  out.push_back(0); out.push_back(0);
  for (int i = 0; i < 8; ++i) out.push_back((uint8_t)(((uint64_t)size >> (i*8)) & 0xFF));
  if (size == 0) { put_u32le(out, 0); return true; }
  size_t nchunks = (size + kChunkSize - 1) / kChunkSize;
  put_u32le(out, (uint32_t)nchunks);
  struct Ch { uint32_t raw=0, comp=0, crc=0; std::vector<uint8_t> p; };
  std::vector<Ch> chs(nchunks);
  if (threads <= 0) threads = default_threads();
  if ((size_t)threads > nchunks) threads = (int)nchunks;
  if (threads <= 1) {
    for (size_t i = 0; i < nchunks; ++i) {
      size_t off = i * kChunkSize, len = size - off < kChunkSize ? size - off : kChunkSize;
      chs[i].raw = (uint32_t)len;
      chs[i].crc = crc32_compute(data + off, len);
      chs[i].p = compress_one_chunk(data + off, len, level);
      chs[i].comp = (uint32_t)chs[i].p.size();
      if (progress) progress(i + 1, nchunks);
    }
  } else {
    ThreadPool pool((size_t)threads);
    std::vector<std::future<void>> fus;
    for (size_t i = 0; i < nchunks; ++i)
      fus.push_back(pool.enqueue([&, i] {
        size_t off = i * kChunkSize;
        size_t len = size - off < (size_t)kChunkSize ? size - off : (size_t)kChunkSize;
        chs[i].raw = (uint32_t)len;
        chs[i].crc = ::kzip::crc32_compute(data + off, len);
        chs[i].p = compress_one_chunk(data + off, len, level);
        chs[i].comp = (uint32_t)chs[i].p.size();
      }));
    for (auto& f : fus) f.get();
    if (progress) progress(nchunks, nchunks);
  }
  for (auto& c : chs) { put_u32le(out, c.raw); put_u32le(out, c.comp); put_u32le(out, c.crc); }
  for (auto& c : chs) out.insert(out.end(), c.p.begin(), c.p.end());
  return true;
}

bool decompress_buffer(const uint8_t* data, size_t size,
                       std::vector<uint8_t>& out, ProgressCb progress) {
  out.clear();
  if (size < 4 + 2 + 2 + 8 + 4) return false;
  if (!(data[0]=='K'&&data[1]=='Z'&&data[2]=='B'&&data[3]==0x01)) return false;
  int level = data[4] | (data[5] << 8);
  uint64_t orig = 0;
  for (int i = 0; i < 8; ++i) orig |= (uint64_t)data[8+i] << (i*8);
  uint32_t nchunks = get_u32le(data + 16);
  const uint8_t* p = data + 20;
  size_t rem = size - 20;
  if (nchunks == 0) return orig == 0;
  if (rem < (size_t)nchunks * 12) return false;
  struct H { uint32_t raw, comp, crc; };
  std::vector<H> hs(nchunks);
  for (uint32_t i = 0; i < nchunks; ++i) {
    hs[i].raw = get_u32le(p); hs[i].comp = get_u32le(p+4); hs[i].crc = get_u32le(p+8);
    p += 12; rem -= 12;
  }
  out.resize((size_t)orig);
  size_t woff = 0;
  for (uint32_t i = 0; i < nchunks; ++i) {
    if (rem < hs[i].comp) return false;
    std::vector<uint8_t> cr;
    if (!decompress_one_chunk(p, hs[i].comp, cr, hs[i].raw, level)) return false;
    if (::kzip::crc32_compute(cr.data(), cr.size()) != hs[i].crc) return false;
    memcpy(out.data() + woff, cr.data(), cr.size());
    woff += cr.size();
    p += hs[i].comp; rem -= hs[i].comp;
    if (progress) progress(i + 1, nchunks);
  }
  return woff == (size_t)orig;
}

// ---------------- Helpers ----------------
static bool is_solid_name(const std::string& n) {
  if (n.compare(0, sizeof(kSolidPrefix) - 1, kSolidPrefix) != 0) return false;
  if (n.size() < sizeof(kSolidPrefix) - 1 + sizeof(kSolidSuffix) - 1) return false;
  return n.compare(n.size() - (sizeof(kSolidSuffix) - 1), sizeof(kSolidSuffix) - 1,
                   kSolidSuffix) == 0;
}

static bool has_solid_entries(const std::vector<ArchiveEntry>& es) {
  for (auto& e : es) if (is_solid_name(e.name)) return true;
  return false;
}

static container::V2Params resolve_v2params(ProfileOpt po) {
  Profile p = Profile::Lite;
  if (po == ProfileOpt::Ultra) p = Profile::Ultra;
  else if (po == ProfileOpt::Lite) p = Profile::Lite;
  else p = auto_profile();
  int H, E;
  profile_hidden_embed(p, H, E);
  container::V2Params vp;
  vp.H = H; vp.E = E;
  vp.profile = (p == Profile::Ultra) ? 2 : 1;
  vp.seed = kSeedV2;
  return vp;
}

// Read the first 64KB of the file for prefilter (don't read the whole large file)
static prefilter::Verdict prefilter_file_head(const solid::ScannedFile& sf) {
  if (sf.size == 0) {
    prefilter::Verdict v; v.should_store = true; v.reason = "empty"; return v;
  }
  // Check magic by extension first (fast, no IO) + read head to check actual magic
  std::ifstream f(sf.fspath, std::ios::binary);
  uint8_t head[65536];
  size_t got = 0;
  if (f) { f.read((char*)head, sizeof(head)); got = (size_t)f.gcount(); }
  if (got == 0) {
    // Unreadable -> keep it to fail loudly in a later step (don't silently STORE)
    prefilter::Verdict v; v.reason = "unreadable"; return v;
  }
  prefilter::Verdict v = prefilter::analyze(head, got);
  // Small file (<64KB): the sample is the whole file -> accurate verdict.
  // Large file: the first-64KB sample is representative (per spec).
  (void)sf;
  return v;
}

// ---------------- compress_archive v2 ----------------
bool compress_archive_ex(const std::vector<std::string>& inputs,
                         const std::string& archive_path,
                         const CompressOptions& opt,
                         ProgressCb progress) {
  lower_process_priority();
  int threads = effective_threads(opt.threads);

  solid::ScannedTree tree;
  if (!solid::scan_inputs(inputs, tree)) return false;
  solid::sort_smart(tree.files);

  container::V2Params vp = resolve_v2params(opt.profile);

  std::vector<container::FileItem> items;
  items.reserve(tree.dirs.size() + tree.files.size() + 8);
  for (auto& d : tree.dirs) {
    container::FileItem it;
    it.arcname = d.arcname; it.fspath = ""; it.is_dir = true;
    it.mtime = d.mtime; it.mode = d.mode ? d.mode : (0040000 | 0755);
    it.size = 0; it.crc = 0; it.method = 0;
    items.push_back(std::move(it));
  }

  // Classify each file with the prefilter
  struct Job { size_t fi; bool store; };
  std::vector<Job> jobs;
  jobs.reserve(tree.files.size());
  std::vector<size_t> compressible;
  for (size_t i = 0; i < tree.files.size(); ++i) {
    auto& sf = tree.files[i];
    prefilter::Verdict v = prefilter_file_head(sf);
    if (v.should_store) jobs.push_back({i, true});
    else { jobs.push_back({i, false}); compressible.push_back(i); }
  }

  // Single loose file (not a folder) and non-solid -> keep the old per-file style
  // (but upgrade the stream to v2 for the profile flag). For simplicity: if total
  // files == 1 and no dirs -> single-file entry with the original name.
  bool single_mode = (tree.files.size() == 1 && tree.dirs.empty());

  if (single_mode) {
    // Single file: find the matching job
    bool store = true;
    for (auto& j : jobs) if (j.fi == 0) store = j.store;
    auto& sf = tree.files[0];
    if (store) {
      std::vector<uint8_t> raw;
      if (!solid::read_file_bytes(sf.fspath, raw)) return false;
      container::FileItem it;
      it.arcname = sf.arcname; it.is_dir = false;
      it.mtime = sf.mtime; it.mode = sf.mode ? sf.mode : (0100000 | 0644);
      it.size = raw.size();
      it.crc = raw.empty() ? 0 : crc32_compute(raw.data(), raw.size());
      it.method = 0;
      it.comp_data = std::move(raw);
      items.push_back(std::move(it));
    } else {
      std::vector<uint8_t> raw;
      if (!solid::read_file_bytes(sf.fspath, raw)) return false;
      container::V2Params vp1 = vp;
      std::vector<uint8_t> stream =
          container::encode_stream_v2(raw.empty() ? nullptr : raw.data(), raw.size(),
                                      vp1, nullptr, threads, nullptr);
      // Fallback: if bloated vs raw then STORE (rare, but guards the ratio)
      if (stream.size() >= raw.size() + 64 && !raw.empty()) {
        container::FileItem it;
        it.arcname = sf.arcname; it.is_dir = false;
        it.mtime = sf.mtime; it.mode = sf.mode ? sf.mode : (0100000 | 0644);
        it.size = raw.size();
        it.crc = crc32_compute(raw.data(), raw.size());
        it.method = 0;
        it.comp_data = std::move(raw);
        items.push_back(std::move(it));
      } else {
        container::FileItem it;
        it.arcname = sf.arcname; it.is_dir = false;
        it.mtime = sf.mtime; it.mode = sf.mode ? sf.mode : (0100000 | 0644);
        it.size = raw.size();
        it.crc = raw.empty() ? 0 : crc32_compute(raw.data(), raw.size());
        it.method = kMethodId;
        it.comp_data = std::move(stream);
        items.push_back(std::move(it));
      }
    }
    if (progress) progress(1, 1);
    return container::write_archive(archive_path, items);
  }

  // Folder mode: separate STORE files + compressible files -> solid groups
  uint64_t done = 0;
  uint64_t total_units = 0;
  // Count: each STORE file = 1 unit, each solid group = 1 unit
  std::vector<solid::ScannedFile> comp_files;
  std::vector<size_t> comp_orig_idx;
  for (auto j : jobs) {
    if (j.store) total_units++;
    else { comp_files.push_back(tree.files[j.fi]); comp_orig_idx.push_back(j.fi); }
  }
  // Pack groups over comp_files (sort order inherited)
  std::vector<solid::SolidGroup> groups;
  if (!comp_files.empty() && opt.use_solid)
    groups = solid::pack_groups(comp_files, opt.solid_target, opt.solid_max);
  else {
    // --no-solid: one group per file
    for (size_t i = 0; i < comp_files.size(); ++i) {
      solid::SolidGroup g; g.idx.push_back(i); g.raw_size = comp_files[i].size;
      groups.push_back(g);
    }
  }
  total_units += groups.size();
  if (total_units == 0) total_units = 1;

  // 1) STORE files
  for (auto j : jobs) {
    if (!j.store) continue;
    auto& sf = tree.files[j.fi];
    std::vector<uint8_t> raw;
    if (!solid::read_file_bytes(sf.fspath, raw)) return false;
    container::FileItem it;
    it.arcname = sf.arcname; it.is_dir = false;
    it.mtime = sf.mtime; it.mode = sf.mode ? sf.mode : (0100000 | 0644);
    it.size = raw.size();
    it.crc = raw.empty() ? 0 : crc32_compute(raw.data(), raw.size());
    it.method = 0;
    it.comp_data = std::move(raw);
    items.push_back(std::move(it));
    if (progress) progress(++done, total_units);
  }

  // 2) Solid groups (parallel across groups, sequential within a group)
  struct SolidOut { size_t gi; std::vector<uint8_t> stream; std::vector<uint8_t> raw_concat;
                    std::vector<solid::ManifestEntry> man; bool fallback_store = false; };
  std::vector<SolidOut> outs(groups.size());

  auto work_group = [&](size_t gi) -> bool {
    auto& g = groups[gi];
    SolidOut so; so.gi = gi;
    std::vector<std::vector<uint8_t>> bufs;
    bufs.reserve(g.idx.size());
    uint64_t sum = 0;
    for (size_t k : g.idx) {
      auto& sf = comp_files[k];
      std::vector<uint8_t> b;
      if (!solid::read_file_bytes(sf.fspath, b)) return false;
      if (b.size() != sf.size) {
        // Size changed between scan and read (file modified) -> use the actual size
      }
      sum += b.size();
      bufs.push_back(std::move(b));
    }
    so.raw_concat.reserve((size_t)sum);
    so.man.reserve(g.idx.size());
    for (size_t t = 0; t < g.idx.size(); ++t) {
      auto& sf = comp_files[g.idx[t]];
      auto& b = bufs[t];
      solid::ManifestEntry e;
      e.path = sf.arcname;
      e.size = b.size();
      e.mtime = sf.mtime;
      e.mode = sf.mode ? sf.mode : (0100000 | 0644);
      e.crc = b.empty() ? 0 : crc32_compute(b.data(), b.size());
      so.man.push_back(e);
      so.raw_concat.insert(so.raw_concat.end(), b.begin(), b.end());
    }
    // Block-level prefilter: if entropy is high -> mark fallback STORE
    // (split back into per-file STORE at merge time so Explorer can still read it)
    if (!so.raw_concat.empty()) {
      prefilter::Verdict bv = prefilter::analyze(so.raw_concat.data(), so.raw_concat.size());
      if (bv.should_store) { so.fallback_store = true; outs[gi] = std::move(so); return true; }
    }
    const uint8_t* d = so.raw_concat.empty() ? nullptr : so.raw_concat.data();
    so.stream = container::encode_stream_v2_solid(d, so.raw_concat.size(), vp, so.man);
    // Bloat fallback: if comp >= raw then STORE
    if (!so.raw_concat.empty() && so.stream.size() >= so.raw_concat.size())
      so.fallback_store = true;
    outs[gi] = std::move(so);
    return true;
  };

  if (threads <= 1) {
    for (size_t gi = 0; gi < groups.size(); ++gi) {
      if (!work_group(gi)) return false;
      if (progress) progress(++done, total_units);
    }
  } else {
    ThreadPool pool((size_t)threads);
    std::vector<std::future<bool>> fus;
    for (size_t gi = 0; gi < groups.size(); ++gi)
      fus.push_back(pool.enqueue([&, gi] { return work_group(gi); }));
    for (size_t gi = 0; gi < fus.size(); ++gi) {
      if (!fus[gi].get()) return false;
      if (progress) progress(++done, total_units);
    }
  }

  // 3) Merge: solid KZ blobs or split fallback STORE
  char namebuf[64];
  size_t solid_no = 0;
  for (auto& so : outs) {
    if (so.fallback_store) {
      // Split the group into per-file STORE entries
      size_t off = 0;
      for (auto& e : so.man) {
        container::FileItem it;
        it.arcname = e.path; it.is_dir = false;
        it.mtime = e.mtime; it.mode = e.mode;
        it.size = e.size; it.crc = e.crc;
        it.method = 0;
        it.comp_data.assign(so.raw_concat.begin() + off,
                            so.raw_concat.begin() + off + (size_t)e.size);
        off += (size_t)e.size;
        items.push_back(std::move(it));
      }
    } else {
      snprintf(namebuf, sizeof(namebuf), "%s%04zu%s", kSolidPrefix, solid_no++,
               kSolidSuffix);
      uint64_t sum = 0;
      uint32_t xor_crc = 0;
      for (auto& e : so.man) { sum += e.size; xor_crc ^= e.crc; }
      (void)xor_crc;
      container::FileItem it;
      it.arcname = namebuf; it.is_dir = false;
      it.mtime = 0; it.mode = (0100000 | 0644);
      // mtime/mode of the solid blob: take max member mtime (for Explorer display)
      for (auto& e : so.man) if (e.mtime > it.mtime) it.mtime = e.mtime;
      it.size = sum; // uncomp = total raw (so Explorer shows the true size)
      // crc of the solid entry: xor total (display only; real check lives in the manifest)
      it.crc = xor_crc;
      it.method = kMethodId;
      it.comp_data = std::move(so.stream);
      items.push_back(std::move(it));
    }
  }

  return container::write_archive(archive_path, items);
}

bool compress_archive(const std::vector<std::string>& inputs,
                      const std::string& archive_path, int level, int threads,
                      ProgressCb progress) {
  CompressOptions o;
  o.level = level; o.threads = threads; o.profile = ProfileOpt::Auto;
  // Keep legacy level behavior for small single files? V2 auto-selects profile by CPU.
  return compress_archive_ex(inputs, archive_path, o, progress);
}

// ---------------- Path safety ----------------
static bool safe_member_path(const std::string& p) {
  if (p.empty() || p.size() > 4096) return false;
  if (p[0] == '/' || p[0] == '\\') return false;
  if (p.find("..") != std::string::npos) {
    // reject "...", ".." as path segments (allow "a..b.txt")
    // check each segment
    size_t i = 0;
    while (i <= p.size()) {
      size_t j = p.find_first_of("/\\", i);
      std::string seg = p.substr(i, j == std::string::npos ? j : j - i);
      if (seg == ".." || seg == ".") return false;
      if (j == std::string::npos) break;
      i = j + 1;
    }
  }
  if (p.find('\0') != std::string::npos) return false;
  return true;
}

// ---------------- decompress / test / list (v1+v2) ----------------
static bool decode_payload_to_raw(const std::vector<uint8_t>& payload, uint16_t method,
                                  std::vector<uint8_t>& raw) {
  if (method == kMethodStore || method == 0) { raw = payload; return true; }
  if (method != kMethodId) return false;
  if (payload.size() >= 4 && payload[0]=='K' && payload[1]=='Z' && payload[2]==0x02) {
    container::V2Params pr;
    std::vector<std::string> pa; std::vector<uint64_t> sz, mt;
    std::vector<uint32_t> mo, cr;
    if (!container::decode_stream_v2(payload.data(), payload.size(), raw, &pr,
                                     &pa, &sz, &mt, &mo, &cr))
      return false;
    return true;
  }
  if (payload.size() >= 4 && payload[0]=='K' && payload[1]=='Z' && payload[2]==0x01) {
    return container::decode_stream(payload.data(), payload.size(), raw);
  }
  return false;
}

bool decompress_archive(const std::string& archive_path, const std::string& out_dir,
                        int threads, ProgressCb progress) {
  (void)threads;
  std::vector<ArchiveEntry> entries;
  std::vector<uint64_t> lhos, cs, us;
  std::vector<uint32_t> crcs;
  std::vector<uint16_t> methods;
  if (!container::read_central(archive_path, entries, lhos, cs, us, crcs, methods))
    return false;
  fs::create_directories(out_dir);
  bool has_solid = has_solid_entries(entries);
  uint64_t done = 0, total = entries.size() ? entries.size() : 1;
  for (size_t i = 0; i < entries.size(); ++i) {
    const std::string& nm = entries[i].name;
    if (entries[i].is_dir) {
      if (!safe_member_path(nm)) return false;
      fs::create_directories(fs::path(out_dir) / nm);
      if (progress) progress(++done, total);
      continue;
    }
    if (has_solid && is_solid_name(nm)) {
      // Extract solid group -> multiple files
      std::vector<uint8_t> payload;
      uint64_t u; uint32_t c; uint16_t m; std::string an;
      if (!container::read_entry_payload(archive_path, lhos[i], payload, u, c, m, an))
        return false;
      if (m != kMethodId) return false;
      std::vector<uint8_t> raw;
      container::V2Params pr;
      std::vector<std::string> pa; std::vector<uint64_t> sz, mt;
      std::vector<uint32_t> mo, cr;
      if (!container::decode_stream_v2(payload.data(), payload.size(), raw, &pr,
                                       &pa, &sz, &mt, &mo, &cr))
        return false;
      size_t off = 0;
      for (size_t k = 0; k < pa.size(); ++k) {
        if (!safe_member_path(pa[k])) return false;
        if (off + sz[k] > raw.size()) return false;
        const uint8_t* fp = raw.data() + off;
        if (cr[k] != 0 || sz[k] != 0) {
          if (crc32_compute(fp, (size_t)sz[k]) != cr[k]) return false;
        }
        fs::path op = fs::path(out_dir) / pa[k];
        fs::create_directories(op.parent_path());
        std::ofstream f(op, std::ios::binary);
        if (!f) return false;
        if (sz[k]) f.write((const char*)fp, (std::streamsize)sz[k]);
        f.close();
#if !defined(_WIN32)
        if (mt[k]) {
          struct timespec ts[2];
          ts[0].tv_sec = (time_t)mt[k]; ts[0].tv_nsec = 0;
          ts[1].tv_sec = (time_t)mt[k]; ts[1].tv_nsec = 0;
          utimensat(AT_FDCWD, op.c_str(), ts, 0);
        }
        if (mo[k]) ::chmod(op.c_str(), (mode_t)(mo[k] & 07777));
#endif
        off += (size_t)sz[k];
      }
      if (off != raw.size()) return false;
      if (progress) progress(++done, total);
      continue;
    }
    // Regular entry (v1 per-file / v2 single / STORE)
    if (!safe_member_path(nm)) return false;
    fs::path op = fs::path(out_dir) / nm;
    std::vector<uint8_t> payload;
    uint64_t u; uint32_t c; uint16_t m; std::string an;
    if (!container::read_entry_payload(archive_path, lhos[i], payload, u, c, m, an))
      return false;
    std::vector<uint8_t> raw;
    if (!decode_payload_to_raw(payload, m, raw)) return false;
    // Verify CRC by stream type
    if (m == 0) {
      if (!raw.empty() && crc32_compute(raw.data(), raw.size()) != c) return false;
    } else if (payload.size() >= 4 && payload[2] == 0x02) {
      // v2 single: empty manifest, zip crc == file crc (checked in manifest? verify zip here)
      if (!raw.empty() && crc32_compute(raw.data(), raw.size()) != c) {
        // solid single may carry the file crc as zip crc -> must match
        return false;
      }
    } else {
      if (!raw.empty() && crc32_compute(raw.data(), raw.size()) != c) return false;
    }
    fs::create_directories(op.parent_path());
    std::ofstream f(op, std::ios::binary);
    if (!f) return false;
    if (!raw.empty()) f.write((char*)raw.data(), raw.size());
    f.close();
#if !defined(_WIN32)
    if (entries[i].mtime) {
      struct timespec ts[2];
      ts[0].tv_sec = (time_t)entries[i].mtime; ts[0].tv_nsec = 0;
      ts[1].tv_sec = (time_t)entries[i].mtime; ts[1].tv_nsec = 0;
      utimensat(AT_FDCWD, op.c_str(), ts, 0);
    }
    if (entries[i].mode) ::chmod(op.c_str(), (mode_t)(entries[i].mode & 07777));
#endif
    if (progress) progress(++done, total);
  }
  return true;
}

bool test_archive(const std::string& archive_path) {
  std::vector<ArchiveEntry> entries;
  std::vector<uint64_t> lhos, cs, us;
  std::vector<uint32_t> crcs;
  std::vector<uint16_t> methods;
  if (!container::read_central(archive_path, entries, lhos, cs, us, crcs, methods))
    return false;
  for (size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].is_dir) continue;
    std::vector<uint8_t> payload;
    uint64_t u; uint32_t c; uint16_t m; std::string an;
    if (!container::read_entry_payload(archive_path, lhos[i], payload, u, c, m, an))
      return false;
    if (m != 0 && m != kMethodId) return false;
    if (is_solid_name(entries[i].name)) {
      std::vector<uint8_t> raw;
      container::V2Params pr;
      std::vector<std::string> pa; std::vector<uint64_t> sz, mt;
      std::vector<uint32_t> mo, cr;
      if (!container::decode_stream_v2(payload.data(), payload.size(), raw, &pr,
                                       &pa, &sz, &mt, &mo, &cr))
        return false;
      size_t off = 0;
      for (size_t k = 0; k < pa.size(); ++k) {
        if (off + sz[k] > raw.size()) return false;
        if (crc32_compute(raw.data() + off, (size_t)sz[k]) != cr[k] && !(sz[k]==0 && cr[k]==0))
          return false;
        off += (size_t)sz[k];
      }
      if (off != raw.size()) return false;
      continue;
    }
    std::vector<uint8_t> raw;
    if (!decode_payload_to_raw(payload, m, raw)) return false;
    if (!raw.empty() && crc32_compute(raw.data(), raw.size()) != c) return false;
  }
  return true;
}

bool list_archive(const std::string& archive_path, std::vector<ArchiveEntry>& entries) {
  entries.clear();
  std::vector<ArchiveEntry> central;
  std::vector<uint64_t> lhos, cs, us;
  std::vector<uint32_t> crcs;
  std::vector<uint16_t> methods;
  if (!container::read_central(archive_path, central, lhos, cs, us, crcs, methods))
    return false;
  // Keep dirs + STORE + single as before; expand solid into logical files
  for (size_t i = 0; i < central.size(); ++i) {
    if (central[i].is_dir) { entries.push_back(central[i]); continue; }
    if (!is_solid_name(central[i].name)) { entries.push_back(central[i]); continue; }
    // solid: peek manifest (no decompression)
    std::vector<uint8_t> payload;
    uint64_t u; uint32_t c; uint16_t m; std::string an;
    if (!container::read_entry_payload(archive_path, lhos[i], payload, u, c, m, an))
      return false;
    container::V2Params pr;
    uint64_t rs = 0; uint32_t nc = 0; bool hm = false;
    std::vector<std::string> pa; std::vector<uint64_t> sz;
    if (!container::peek_stream_v2(payload.data(), payload.size(), pr, rs, nc, hm, pa, sz))
      return false;
    // Fetch the full manifest for mtime/mode/crc
    std::vector<uint8_t> dummy;
    std::vector<std::string> p2; std::vector<uint64_t> s2, mt;
    std::vector<uint32_t> mo, cr;
    // peek already yields paths/sizes; read extra meta via header-only decode?
    // Simple path: use peek + read manifest via decode_stream_v2 header?
    // To avoid heavy decompression, read the manifest directly:
    std::vector<solid::ManifestEntry> man;
    {
      const uint8_t* d = payload.data();
      // header 22 + mlen
      if (payload.size() >= 26) {
        uint32_t mlen = (uint32_t)d[22] | ((uint32_t)d[23] << 8) |
                        ((uint32_t)d[24] << 16) | ((uint32_t)d[25] << 24);
        if (26 + mlen <= payload.size()) {
          size_t cons = 0;
          solid::decode_manifest(d + 26, mlen, man, cons);
        }
      }
    }
    for (size_t k = 0; k < pa.size(); ++k) {
      ArchiveEntry e;
      e.name = pa[k];
      e.uncomp_size = k < sz.size() ? sz[k] : 0;
      e.comp_size = 0; // not attributed per file (shared solid)
      e.method = kMethodId;
      e.is_dir = false;
      e.solid_group = central[i].name;
      e.profile = pr.profile;
      if (k < man.size()) {
        e.mtime = man[k].mtime; e.mode = man[k].mode; e.crc32 = man[k].crc;
      }
      entries.push_back(e);
    }
  }
  return true;
}

} // namespace kzip
