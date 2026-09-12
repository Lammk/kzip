# kzip

kzip is a lossless file compression format and toolchain. It combines an
online micro-RNN byte predictor with a 32-bit rANS entropy coder, packed in a
ZIP-compatible container (compression method `0x4B5A`), so classic ZIP tools
can still list archives.

- **Solid blocks**: files are sorted by type and grouped into 8–32MB blocks
  with a binary manifest, so the predictor keeps context across small files.
- **Pre-filter gate**: already-compressed data (JPEG/PNG/ZIP/…) or
  high-entropy blocks (Shannon H ≥ 7.92) are stored uncompressed.
- **Two profiles**: `--lite` (fast, ~42k params) and `--ultra` (thorough,
  ~261k params), with SIMD dispatch (SSE4.1/AVX2+FMA/NEON/DotProd).
- **CLI + C library + 7-Zip plugin** (`kzip.dll` / `kzip.so`).

## Build

Requirements: CMake ≥ 3.16 and a C++17 compiler (GCC, Clang, or MSVC).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

## Usage

```bash
./build/cli/kzip a docs.kzip ./docs -t 4 --ultra   # compress
./build/cli/kzip l docs.kzip                        # list files
./build/cli/kzip t docs.kzip                        # integrity check
./build/cli/kzip x docs.kzip -o ./out               # extract
```

## License

MIT — see [LICENSE](LICENSE).
