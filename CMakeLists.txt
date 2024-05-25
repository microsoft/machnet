cmake_minimum_required(VERSION 3.0)

project(juggler VERSION 0.0.1 DESCRIPTION "Packet Juggling framework in C++" LANGUAGES CXX C)

find_package(gflags REQUIRED)

include(CTest)
set(BUILD_TESTING OFF) # Disable testing for third-party modules

# Common third-party modules

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

set(BUILD_TESTING ON)
option(DEBUG "Enable debug mode" OFF)
if(DEBUG)
    add_definitions(-DkTESTING=1)
    message(STATUS "Debug mode is ON")
else()
    add_definitions(-DkTESTING=0)
    message(STATUS "Debug mode is OFF")
endif()
add_subdirectory(src)
