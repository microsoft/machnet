add_executable (l2fwd main.cc)

target_link_libraries(l2fwd LINK_PUBLIC core glog ${LIBDPDK_LIBRARIES})
