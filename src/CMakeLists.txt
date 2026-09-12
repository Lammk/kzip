add_library(kziplib STATIC
  simd_matvec.cpp
  predictor.cpp
  profile.cpp
  prefilter.cpp
  solid.cpp
  rans.cpp
  crc32.cpp
  sha256.cpp
  chunk.cpp
  container.cpp
  api.cpp
  c_api.cpp
)
target_include_directories(kziplib PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(kziplib PUBLIC cxx_std_17)
if (KZIP_ENABLE_AVX2 AND (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i686"))
  target_compile_options(kziplib PRIVATE -mavx2)
  target_compile_definitions(kziplib PRIVATE __AVX2__=1)
endif()
if (MSVC)
  target_compile_options(kziplib PRIVATE /O2 /arch:AVX2)
endif()
set_target_properties(kziplib PROPERTIES POSITION_INDEPENDENT_CODE ON OUTPUT_NAME kzip)
