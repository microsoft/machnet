set(target_name msg_gen)
add_executable (${target_name} main.cc)

if (WIN32)
target_link_libraries(${target_name} PUBLIC glog gflags machnet_shim hdr_histogram_static ${LIBDPDK_LIBRARIES}) # rt breaks the build
else ()
target_link_libraries(${target_name} PUBLIC glog machnet_shim rt hdr_histogram ${LIBDPDK_LIBRARIES})
endif ()
