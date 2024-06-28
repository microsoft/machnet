cmake_minimum_required(VERSION 3.0)
project(HelloWorld VERSION 1.0)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include_directories(${PROJECT_SOURCE_DIR}/../src/ext)
include_directories(${PROJECT_SOURCE_DIR}/../src/ext/windows)
include_directories(SYSTEM ${PROJECT_SOURCE_DIR}/../build/third_party/gflags/include)
link_directories(${PROJECT_SOURCE_DIR}/../build)

add_executable(hello_world hello_world.cc)

target_link_libraries(hello_world machnet_shim gflags ${LIBDPDK_LIBRARIES})