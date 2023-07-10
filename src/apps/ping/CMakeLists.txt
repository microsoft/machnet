set(target_name ping)
add_executable (${target_name} main.cc)
target_link_libraries(${target_name} LINK_PUBLIC core glog ${LIBDPDK_LIBRARIES})
