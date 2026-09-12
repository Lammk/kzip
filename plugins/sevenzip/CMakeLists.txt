add_library(kzip_7zip SHARED kzip_plugin.cpp)
target_link_libraries(kzip_7zip PRIVATE kziplib)
target_include_directories(kzip_7zip PRIVATE ${CMAKE_SOURCE_DIR}/include)
set_target_properties(kzip_7zip PROPERTIES
  OUTPUT_NAME kzip
  POSITION_INDEPENDENT_CODE ON
)
