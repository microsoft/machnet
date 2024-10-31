set(target_name mongoose)
add_executable (${target_name} main.cc mongoose.cc)
target_link_libraries(${target_name} PUBLIC glog machnet_shim rt hdr_histogram gflags)
target_compile_definitions(${target_name} PUBLIC MACHNET)