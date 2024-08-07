cmake_minimum_required(VERSION 3.0)

project(juggler VERSION 0.0.1 DESCRIPTION "Packet Juggling framework in C++" LANGUAGES CXX C)

include(CTest)
set(BUILD_TESTING OFF) # Disable testing for third-party modules
# SET(CMAKE_FIND_LIBRARY_SUFFIXES .a .so ${CMAKE_FIND_LIBRARY_SUFFIXES})

# SET(BUILD_STATIC_LIBS ON)
SET(BUILD_gflags_nothreads_LIBS ON)
SET(BUILD_gflags_LIBS ON)
add_subdirectory(third_party/gflags)
include_directories(SYSTEM "${CMAKE_CURRENT_BINARY_DIR}/third_party/gflags/include/")

find_package(gflags)

# Glog doesn't correctly define its include directories, so we need to specify
# manually
add_subdirectory(third_party/glog)
include_directories(SYSTEM third_party/glog/src)
include_directories(SYSTEM ${CMAKE_BINARY_DIR}/third_party/glog)

add_subdirectory(third_party/googletest)
include_directories(SYSTEM third_party/googletest/googlemock/include)

# Build google benchmark (target: benchmark)
# do not build tests of benchmarking lib
set(BENCHMARK_ENABLE_INSTALL OFF)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "Suppressing benchmark's tests" FORCE)
add_subdirectory(third_party/googlebench)
include_directories(SYSTEM third_party/googlebench/include)

# Common sub-projects: HdrHistogram
set(HDR_HISTOGRAM_BUILD_PROGRAMS OFF CACHE BOOL "Minimize HDR histogram build")
set(HDR_LOG_REQUIRED OFF CACHE BOOL "Disable HDR histogram's log to avoid zlib dependency")
add_subdirectory(third_party/HdrHistogram_c)
include_directories(SYSTEM third_party/HdrHistogram_c/include/)

include_directories(SYSTEM third_party/xxHash)

# if(NOT gflags_FOUND)
# endif()

set(BUILD_TESTING ON)
add_subdirectory(src)
