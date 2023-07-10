set(target_name nsaas)
add_executable (${target_name} main.cc)
target_link_libraries(${target_name} PUBLIC core glog rt ${LIBDPDK_LIBRARIES})
