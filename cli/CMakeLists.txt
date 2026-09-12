add_executable(kzip main.cpp)
target_link_libraries(kzip PRIVATE kziplib)
target_include_directories(kzip PRIVATE ${CMAKE_SOURCE_DIR}/include)
install(TARGETS kzip RUNTIME DESTINATION bin)
