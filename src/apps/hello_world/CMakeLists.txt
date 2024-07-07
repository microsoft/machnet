set(target_name hello_world)
add_executable (${target_name} main.cc)

if (WIN32)
    target_link_libraries(${target_name} PUBLIC glog core machnet_shim gflags ${LIBDPDK_LIBRARIES}) # rt breaks the build
else ()
    target_link_libraries(${target_name} PUBLIC glog core machnet_shim rt ${LIBDPDK_LIBRARIES})
endif ()
