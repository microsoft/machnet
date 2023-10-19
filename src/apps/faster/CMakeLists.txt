set(target_name faster_server)

# add_executable (${target_name} faster_server.cc)
# target_link_libraries(${target_name} LINK_PUBLIC core glog ${LIBDPDK_LIBRARIES})

# Try to find a FASTER installation
set(FASTER_INSTALL_DIR /home/alireza/FASTER/build/Release)
list(APPEND CMAKE_PREFIX_PATH ${FASTER_INSTALL_DIR}/)
if (NOT EXISTS ${FASTER_INSTALL_DIR})
    message(WARNING "Faster installation not found at ${FASTER_INSTALL_DIR}. Not building faster_server")
    return()
endif()

message(WARNING "Found faster ${CMAKE_PREFIX_PATH} ${FASTER_INSTALL_DIR}/../../cc/src/")

# find_package(FASTER REQUIRED)
include_directories(SYSTEM ${FASTER_INSTALL_DIR}/../../cc/src/)
include_directories(SYSTEM ${FASTER_INSTALL_DIR}/../../cc/test/)
add_executable (${target_name} faster_server.cc)
target_link_libraries(${target_name} PUBLIC glog core machnet_shim rt hdr_histogram tbb ${LIBDPDK_LIBRARIES} ${CMAKE_PREFIX_PATH}/libfaster.a)


set (target_name faster_client)
add_executable (${target_name} faster_client.cc)
target_link_libraries(${target_name} PUBLIC glog machnet_shim rt hdr_histogram ${LIBDPDK_LIBRARIES})

