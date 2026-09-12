#pragma once
// 7-Zip plugin for kzip (Method ID 0x4B5A, v1+v2/profile auto).
// Inherits the ICompressCoder interface from the 7-Zip SDK:
//
//   class ICompressCoder : public IUnknown {
//     virtual HRESULT Code(ISequentialInStream*, ISequentialOutStream*,
//                          const UInt64*, const UInt64*, ICompressProgressInfo*) = 0;
//   };
//
// Standalone build to check ABI; when the real SDK is available, wire up KzipCoder below.
#include <cstdint>
#include <vector>

namespace kzip { namespace sevenzip {

static constexpr uint32_t kMethodId = 0x4B5A;

// profile: 0 auto, 1 lite, 2 ultra (default auto by CPU)
bool kzip_compress_stream(const uint8_t* in, size_t in_n,
                          std::vector<uint8_t>& out, int profile = 0);
bool kzip_decompress_stream(const uint8_t* in, size_t in_n,
                            std::vector<uint8_t>& out, size_t out_hint = 0);

#if defined(KZIP_HAVE_7ZIP_SDK)
// Sample glue when the 7-Zip SDK is available (C++):
/*
#include "CPP/7zip/ICoder.h"
#include <kzip/c_api.h>
class KzipCoder : public ICompressCoder, public CMyUnknownImp {
  int profile_ = 0; // 0 auto, 1 lite, 2 ultra
  MY_UNKNOWN_IMP1(ICompressCoder)
  STDMETHOD(Code)(ISequentialInStream* in, ISequentialOutStream* out,
                  const UInt64* inSize, const UInt64* outSize,
                  ICompressProgressInfo* progress) override {
    // Read all of in -> buf; ::kzip_compress_stream (encode) or
    // ::kzip_decompress_stream (decode) depending on _encodeMode; write to out.
    // Register MethodID 0x4B5A in RegisterCodec so 7-Zip can open/decompress natively.
  }
};
*/
#endif

}} // namespace kzip::sevenzip
