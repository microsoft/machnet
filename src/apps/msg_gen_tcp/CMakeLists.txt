set(target_name msg_gen_tcp)
add_executable(${target_name} main.cc)
target_link_libraries(${target_name} PUBLIC glog machnet_shim rt hdr_histogram ${LIBDPDK_LIBRARIES})
