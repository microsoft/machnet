set(target_name rocksdb_server)

# Try to find a RocksDB installation
set(ROCKSDB_INSTALL_DIR /mnt/ankalia/rocksdb/local_install)
list(APPEND CMAKE_PREFIX_PATH ${ROCKSDB_INSTALL_DIR}/lib/cmake/rocksdb)
if (NOT EXISTS ${ROCKSDB_INSTALL_DIR}/lib/cmake/rocksdb)
    message(WARNING "RocksDB installation not found at ${ROCKSDB_INSTALL_DIR}. Not building rocksdb_server example app.")
    return()
endif()

find_package(RocksDB REQUIRED)
add_executable (${target_name} rocksdb_server.cc)
target_link_libraries(${target_name} PUBLIC glog core machnet_shim rt ${LIBDPDK_LIBRARIES} RocksDB::rocksdb)
