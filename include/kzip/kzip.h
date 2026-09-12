#pragma once
// Public API of libkzip (v1 compatible + v2 solid/profile).
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "config.h"

namespace kzip {

using ProgressCb = std::function<void(uint64_t done, uint64_t total)>;

enum class ProfileOpt : uint8_t { Auto = 0, Lite = 1, Ultra = 2 };

struct CompressOptions {
  int level = 1;                 // v1 legacy (when profile==Auto and single input)
  int threads = 0;               // 0 = auto (effective_threads)
  ProfileOpt profile = ProfileOpt::Auto; // --lite / --ultra
  uint64_t solid_target = kSolidTarget;  // 8MB
  uint64_t solid_max = kSolidMax;        // 32MB
  bool use_solid = true;         // folder -> solid; file don -> single
};

// --- Buffer API v1 (frozen for old archive/buffer compatibility) ---
bool compress_buffer(const uint8_t* data, size_t size,
                     std::vector<uint8_t>& out,
                     int level = 1, int threads = 0,
                     ProgressCb progress = {});

bool decompress_buffer(const uint8_t* data, size_t size,
                       std::vector<uint8_t>& out,
                       ProgressCb progress = {});

// --- Archive API v2 (solid + prefilter + profile), also reads v1 ---
bool compress_archive(const std::vector<std::string>& inputs,
                      const std::string& archive_path,
                      int level = 1, int threads = 0,
                      ProgressCb progress = {});

bool compress_archive_ex(const std::vector<std::string>& inputs,
                         const std::string& archive_path,
                         const CompressOptions& opt,
                         ProgressCb progress = {});

bool decompress_archive(const std::string& archive_path,
                        const std::string& out_dir,
                        int threads = 0,
                        ProgressCb progress = {});

bool test_archive(const std::string& archive_path);

struct ArchiveEntry {
  std::string name;
  uint64_t uncomp_size = 0;
  uint64_t comp_size = 0;
  uint32_t crc32 = 0;
  uint16_t method = 0;
  bool is_dir = false;
  uint64_t mtime = 0;
  uint32_t mode = 0;
  // v2 extension (empty for regular entries; set for files inside solid)
  std::string solid_group; // "" if not a solid member
  uint8_t profile = 0;     // 0=unknown, 1=Lite, 2=Ultra
};
bool list_archive(const std::string& archive_path,
                  std::vector<ArchiveEntry>& entries);

// Utilities
uint32_t crc32_compute(const uint8_t* data, size_t n);
std::string sha256_hex(const uint8_t* data, size_t n);
std::string sha256_hex(const std::vector<uint8_t>& v);
int default_threads();
int effective_threads(int requested);
int param_count_for_level(int level);
int param_count_for_profile(int profile_id); // 1 Lite, 2 Ultra
const char* profile_name_of(int profile_id);

} // namespace kzip
