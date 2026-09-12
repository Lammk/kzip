#pragma once
// ZIP-compatible container with Method 0x4B5A.
//  v1 legacy: 1 KZIP stream per file (magic 01), per-chunk reset.
//  v2 solid : folder -> solid groups __kzip/solid-XXXX.kzb (magic 02,
//             continuous predictor seed 1337 + profile flag + binary manifest),
//             incompressible files -> separate STORE (method 0, instantly readable).
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "../include/kzip/kzip.h"
#include "solid.h"

namespace kzip { namespace container {

static constexpr uint32_t kSigLocal = 0x04034b50u;
static constexpr uint32_t kSigCentral = 0x02014b50u;
static constexpr uint32_t kSigEocd = 0x06054b50u;

static constexpr uint8_t kStreamMagicV1[4] = {'K', 'Z', 0x01, 0x00};
static constexpr uint8_t kStreamMagicV2[4] = {'K', 'Z', 0x02, 0x00};

// v2 stream header:
// [K,Z,02,00][u16 H][u16 E][u8 profile 1=Lite 2=Ultra][u8 flags bit0=HAS_MANIFEST]
// [u32 seed][u64 raw_size][u32 nchunks]
// if HAS_MANIFEST: [binary manifest: u32 nfiles + entries]
// then: [u32 raw][u32 comp][u32 crc] * nchunks + chunk payloads (continuous)
static constexpr uint8_t kFlagManifest = 0x01;

struct FileItem {
  std::string arcname; // UTF-8, uses '/' for dirs
  std::string fspath;  // real path (when compressing); empty when decompressing
  bool is_dir = false;
  uint64_t mtime = 0;
  uint32_t mode = 0; // st_mode
  uint64_t size = 0;
  uint32_t crc = 0;
  std::vector<uint8_t> comp_data; // KZIP stream (v1) or raw (STORE)
  uint16_t method = 0;            // 0 or 0x4B5A (for writing local header)
};

void put_u16le(std::vector<uint8_t>& o, uint16_t v);
void put_u32le(std::vector<uint8_t>& o, uint32_t v);
void put_u64le(std::vector<uint8_t>& o, uint64_t v);
uint16_t get_u16le(const uint8_t* p);
uint32_t get_u32le(const uint8_t* p);
uint64_t get_u64le(const uint8_t* p);

void unix_to_dos(uint64_t unix_time, uint16_t& dos_time, uint16_t& dos_date);
uint64_t dos_to_unix(uint16_t dos_time, uint16_t dos_date);

// --- v1 (legacy, kept to read old archives + compress_buffer) ---
std::vector<uint8_t> encode_stream(const uint8_t* data, size_t n, int level,
                                   int threads);
bool decode_stream(const uint8_t* data, size_t n, std::vector<uint8_t>& raw_out);

// --- v2 ---
struct V2Params {
  int H = 96, E = 24;
  uint8_t profile = 1; // 1 Lite, 2 Ultra
  uint32_t seed = 1337;
};
// Compress one contiguous buffer (single-file: empty manifest; solid: with manifest)
std::vector<uint8_t> encode_stream_v2(const uint8_t* data, size_t n,
                                      const V2Params& pr,
                                      const std::vector<std::pair<std::string,
                                        std::vector<uint8_t>>>* manifest_opt,
                                      int threads,
                                      const std::vector<std::pair<uint64_t,uint32_t>>* meta_opt);
// Solid with binary manifest (used directly, avoids JSON parsing)
struct SolidManifestAlias;
std::vector<uint8_t> encode_stream_v2_solid(const uint8_t* data, size_t n,
                                            const V2Params& pr,
                                            const std::vector<::kzip::solid::ManifestEntry>& man);
// Decode v2, returns raw concat + manifest (if any)
bool decode_stream_v2(const uint8_t* data, size_t n,
                      std::vector<uint8_t>& raw_out,
                      V2Params* pr_out,
                      std::vector<std::string>* paths_out,
                      std::vector<uint64_t>* sizes_out,
                      std::vector<uint64_t>* mtimes_out,
                      std::vector<uint32_t>* modes_out,
                      std::vector<uint32_t>* crcs_out);
// Quick v2 header peek (no decompression) for `kzip l`
bool peek_stream_v2(const uint8_t* data, size_t n, V2Params& pr,
                    uint64_t& raw_size, uint32_t& nchunks, bool& has_manifest,
                    std::vector<std::string>& paths, std::vector<uint64_t>& sizes);

bool write_archive(const std::string& path, std::vector<FileItem>& items);
// Load the whole archive file once (64-bit clean); _from_buf variants below
// parse from memory so x/t/l read the file a single time, not once per entry.
bool load_archive_file(const std::string& path, std::vector<uint8_t>& out);
bool read_central(const std::string& path, std::vector<ArchiveEntry>& entries,
                  std::vector<uint64_t>& lho_offsets,
                  std::vector<uint64_t>& comp_sizes,
                  std::vector<uint64_t>& uncomp_sizes,
                  std::vector<uint32_t>& crcs,
                  std::vector<uint16_t>& methods);
bool read_central_from_buf(const std::vector<uint8_t>& buf,
                           std::vector<ArchiveEntry>& entries,
                           std::vector<uint64_t>& lho_offsets,
                           std::vector<uint64_t>& comp_sizes,
                           std::vector<uint64_t>& uncomp_sizes,
                           std::vector<uint32_t>& crcs,
                           std::vector<uint16_t>& methods);
bool read_entry_payload(const std::string& path, uint64_t lho_offset,
                        std::vector<uint8_t>& payload,
                        uint64_t& uncomp_size, uint32_t& crc, uint16_t& method,
                        std::string& arcname);
bool read_entry_payload_from_buf(const std::vector<uint8_t>& buf, uint64_t lho_offset,
                                 std::vector<uint8_t>& payload,
                                 uint64_t& uncomp_size, uint32_t& crc, uint16_t& method,
                                 std::string& arcname);

}} // namespace kzip::container
