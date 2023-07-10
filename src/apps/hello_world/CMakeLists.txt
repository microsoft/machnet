set(target_name hello_world)
add_executable (${target_name} main.cc)
target_link_libraries(${target_name} PUBLIC glog core nsaas_shim rt ${LIBDPDK_LIBRARIES})
