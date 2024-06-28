set(target_name machnet)
add_executable (${target_name} main.cc)
target_link_libraries(${target_name} PUBLIC core glog rt gflags::gflags)
target_include_directories(${target_name} PUBLIC ${GFLAGS_INCLUDE_DIR})
