#pragma once
// Filesystem Layer & Solid Blocks:
//  - Scan std::filesystem::recursive_directory_iterator, normalize to POSIX '/'
//  - Store metadata: size, mode, mtime
//  - Pre-sorting: group by extension + directory + name (helps RNN keep context)
//  - Chunked Solid Blocks 8MB-32MB + binary Manifest
#include <cstdint>
#include <string>
#include <vector>

namespace kzip { namespace solid {

struct ScannedFile {
  std::string arcname;  // relative POSIX, dir ends with '/'
  std::string fspath;   // real path (empty if manifest-only)
  bool is_dir = false;
  uint64_t size = 0;
  uint64_t mtime = 0;
  uint32_t mode = 0;
  std::string ext;      // lowercase extension ("" if none)
  bool incompressible_hint = false; // by extension
};

struct ScannedTree {
  std::vector<ScannedFile> files; // files only (no dirs)
  std::vector<ScannedFile> dirs;  // dirs only
};

// Scan inputs -> tree (POSIX normalize). Returns false if input does not exist.
bool scan_inputs(const std::vector<std::string>& inputs, ScannedTree& out);

// Smart sort: ext -> parent dir -> name
void sort_smart(std::vector<ScannedFile>& files);

// 1 solid group = set of files concatenated raw then compressed with one model
struct SolidGroup {
  std::vector<size_t> idx; // indices into ScannedTree::files (already sorted)
  uint64_t raw_size = 0;
};

// Group compressible files into groups [target=8MB, split when exceeding max=32MB].
// A single file > max gets its own group.
std::vector<SolidGroup> pack_groups(const std::vector<ScannedFile>& files,
                                    uint64_t target = 8ull << 20,
                                    uint64_t maxb = 32ull << 20);

// Binary manifest inside the solid stream (after v2 header):
// [u32 nfiles] + per file [u16 path_len][path][u64 size][u64 mtime][u32 mode][u32 crc]
struct ManifestEntry {
  std::string path;
  uint64_t size = 0;
  uint64_t mtime = 0;
  uint32_t mode = 0;
  uint32_t crc = 0;
};
void encode_manifest(const std::vector<ManifestEntry>& m, std::vector<uint8_t>& out);
bool decode_manifest(const uint8_t* data, size_t n, std::vector<ManifestEntry>& m,
                     size_t& consumed);

// Read 1 file's bytes (helper)
bool read_file_bytes(const std::string& p, std::vector<uint8_t>& out);
uint32_t file_crc32(const std::string& p, uint64_t* size_out = nullptr);

}} // namespace kzip::solid
