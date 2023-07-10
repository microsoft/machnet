add_executable (echo main.cc)

target_link_libraries(echo LINK_PUBLIC core glog ${LIBDPDK_LIBRARIES})
