set(target_name faster_server)

# Try to find a FASTER installation
set(FASTER_INSTALL_DIR ${PROJECT_BINARY_DIR}/third_party/FASTER/cc)
list(APPEND CMAKE_PREFIX_PATH ${FASTER_INSTALL_DIR})
if (NOT EXISTS ${FASTER_INSTALL_DIR})
    message(WARNING "Faster installation not found at ${FASTER_INSTALL_DIR}. Not building faster_server")
    return()
endif()

message(INFO " Found faster ${CMAKE_PREFIX_PATH} ${FASTER_INSTALL_DIR}")

# find_package(FASTER REQUIRED)
add_executable (${target_name} faster_server.cc)
target_link_libraries(${target_name} PUBLIC glog core machnet_shim rt hdr_histogram tbb ${LIBDPDK_LIBRARIES} ${CMAKE_PREFIX_PATH}/src/libfaster.a)

set (target_name faster_client)
add_executable (${target_name} faster_client.cc)
target_link_libraries(${target_name} PUBLIC glog machnet_shim rt hdr_histogram ${LIBDPDK_LIBRARIES})

set (target_name faster_udp_client)
add_executable (${target_name} faster_udp_client.cc)
target_link_libraries(${target_name} PUBLIC glog machnet_shim rt hdr_histogram ${LIBDPDK_LIBRARIES})

set (target_name faster_tcp)
add_executable (${target_name} faster_tcp.cc)
target_link_libraries(${target_name} PUBLIC glog machnet_shim rt hdr_histogram ${LIBDPDK_LIBRARIES})

