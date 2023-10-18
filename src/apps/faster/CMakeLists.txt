set(target_name faster_server)

add_executable (${target_name} faster_server.cc)
target_link_libraries(${target_name} LINK_PUBLIC core glog ${LIBDPDK_LIBRARIES})

# # Try to find a FASTER installation
# set(FASTER_INSTALL_DIR /mnt/ankalia/rocksdb/local_install)
# list(APPEND CMAKE_PREFIX_PATH ${FASTER_INSTALL_DIR}/lib/cmake/rocksdb)
# if (NOT EXISTS ${FASTER_INSTALL_DIR}/lib/cmake/faster)
#     message(WARNING "Faster installation not found at ${FASTER_INSTALL_DIR}. Not building faster_server")
#     return()
# endif()

# find_package(FASTER REQUIRED)
# add_executable (${target_name} faster_server.cc)
# target_link_libraries(${target_name} PUBLIC glog core machnet_shim rt ${LIBDPDK_LIBRARIES} RocksDB::rocksdb)
