#pragma once
#include <cstdint>

#define KZIP_VERSION_MAJOR 0
#define KZIP_VERSION_MINOR 2
#define KZIP_VERSION_PATCH 0
#define KZIP_VERSION_STRING "0.2.0"

namespace kzip {
// Proprietary Compression Method ID: ASCII 'KZ' -> spec picks 0x4B5A
static constexpr uint16_t kMethodId = 0x4B5A;
static constexpr uint16_t kMethodStore = 0;

// v1 (legacy, per-file, per-chunk reset)
static constexpr uint32_t kChunkSize = 1u << 20;      // 1 MiB
static constexpr uint32_t kChunkSizeLarge = 2u << 20; // 2 MiB (optional)
static constexpr uint32_t kSliceSize = 1u << 14;      // 16 KiB (rANS unit)
static constexpr uint32_t kProbBits = 16;
static constexpr uint32_t kProbScale = 1u << 16; // 65536
static constexpr uint32_t kRansL = 1u << 23;
static constexpr uint32_t kSeedV1 = 0x4B5A5A4Bu;

// v2 (solid): unified seed 1337, 8-32MB blocks
static constexpr uint32_t kSeedV2 = 1337;
static constexpr uint64_t kSolidTarget = 8ull << 20;  // 8 MiB
static constexpr uint64_t kSolidMax = 32ull << 20;    // 32 MiB
static constexpr uint32_t kSolidNameWidth = 4;        // solid-0000
static constexpr char kSolidPrefix[] = "__kzip/solid-";
static constexpr char kSolidSuffix[] = ".kzb";

// Pre-filter gate
static constexpr size_t kEntropySample = 64u << 10; // 64 KiB
static constexpr double kEntropyThreshold = 7.92;   // /8.0 -> STORE

// Stream magics
static constexpr uint8_t kMagicV1[4] = {'K', 'Z', 0x01, 0x00};
static constexpr uint8_t kMagicV2[4] = {'K', 'Z', 0x02, 0x00};
static constexpr uint8_t kMagicBufV1[4] = {'K', 'Z', 'B', 0x01};
} // namespace kzip
