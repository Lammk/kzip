#pragma once
// Pre-Filter Gatekeeper: O(1) Magic Bytes filter + fast Shannon Entropy check (64KB sample).
// If already-compressed/encrypted format matches or H >= 7.92 -> STORE (Method 0), skip AI.
#include <cstddef>
#include <cstdint>
#include <string>

namespace kzip { namespace prefilter {

struct Verdict {
  bool should_store = false; // true -> STORE, do not activate AI
  double entropy = 0.0;      // H(X) over the sample
  size_t sampled = 0;        // actual sampled byte count
  const char* reason = "compressible"; // "magic:<type>" | "entropy>=7.92" | ...
};

// Check magic bytes O(1). Returns the type name on match, nullptr otherwise.
const char* match_magic(const uint8_t* data, size_t n);

// Compute Shannon entropy over the sample (first 64KB max). Uses c*log2(c) LUT.
double shannon_entropy_sample(const uint8_t* data, size_t n, size_t* sampled_out = nullptr);

// Combined verdict for one independent block/file.
Verdict analyze(const uint8_t* data, size_t n);

// Utility: quick guess by extension (supports solid packing decision)
bool extension_likely_compressed(const std::string& path_or_ext);

}} // namespace kzip::prefilter
