set(target_name machnet)
add_executable (${target_name} main.cc)


if (WIN32)
    target_link_libraries(${target_name} PUBLIC core glog ${LIBDPDK_LIBRARIES}) # rt breaks the build
else ()
    target_link_libraries(${target_name} PUBLIC core glog rt ${LIBDPDK_LIBRARIES})
endif ()
