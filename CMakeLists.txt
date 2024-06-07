cmake_minimum_required(VERSION 3.0)
project(juggler VERSION 0.0.1 DESCRIPTION "Packet Juggling framework in C++" LANGUAGES CXX C)
include(CMakeDependentOption)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
set(BUILD_TESTING OFF) # Disable testing for third-party modules

# # Parse the build type [not needed for now]
# if(PERF)
#   message(STATUS "Compilation optimized for performance.")
#   message(STATUS "Debugging is disabled.")
#   set(CMAKE_BUILD_TYPE RelWithDebInfo)
#   set(TESTING OFF)
# else(PERF)
#   message(STATUS "Compilation not optimized for performance.")
#   message(STATUS "Debugging is enabled. Perf will be low.")
#   set(CMAKE_BUILD_TYPE Debug)
#   SET(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0")

#   # For now, disable unit tests on Windows
#   if (NOT WIN32)
#     set(TESTING ON)
#   endif()
# endif(PERF)

# # Common sub-projects: HdrHistogram [not neded for now - will use when measuring latency]
set(HDR_HISTOGRAM_BUILD_PROGRAMS OFF CACHE BOOL "Minimize HDR histogram build")
set(HDR_LOG_REQUIRED OFF CACHE BOOL "Disable HDR histogram's log to avoid zlib dependency")
add_subdirectory(third_party/HdrHistogram_c)
include_directories(SYSTEM third_party/HdrHistogram_c/include/)

# # Common sub-projects: gtest [not needed for now - will use when testing is required]
add_subdirectory(third_party/googletest)
include_directories(SYSTEM third_party/googletest/googlemock/include)

# Common sub-projects: gflags
# add_subdirectory(third_party/gflags)

find_package(gflags REQUIRED)
set(GFLAGS_USE_TARGET_NAMESPACE ON)
find_package(gflags CONFIG REQUIRED)
# target_link_libraries(main PRIVATE gflags::gflags)

# Common sub-projects: asio
include_directories(SYSTEM third_party/asio/include)

# set(gflags_DIR third_party/gflags)

# Glog doesn't correctly define its include directories, so we need to specify manually [not needed for now - will use when encountering error]
add_definitions(-DGLOG_USE_GLOG_EXPORT)
add_subdirectory(third_party/glog)
include_directories(SYSTEM third_party/glog/src)
include_directories(SYSTEM ${CMAKE_BINARY_DIR}/third_party/glog)
# find_package(glog CONFIG REQUIRED)

# Build google benchmark (target: benchmark) do not build tests of benchmarking lib  [not needed for now - will use when encountering error]
set(BENCHMARK_ENABLE_INSTALL OFF)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "Suppressing benchmark's tests" FORCE)
add_subdirectory(third_party/benchmark)
include_directories(SYSTEM third_party/benchmark/include)


# include_directories(SYSTEM third_party/xxHash)
find_package(xxHash CONFIG REQUIRED)
include_directories(SYSTEM C:/cygwin64/home/rr-windows-vm/vcpkg/packages/xxhash_x64-windows/include)


# Add additional compilation flags only after adding subprojects
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -march=native -Wall -Wextra -Werror -pedantic")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wsign-conversion -Wold-style-cast -Wno-unused-function")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-nested-anon-types -Wno-keyword-macro -Wno-deprecated-declarations")

include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(SYSTEM ${CMAKE_SOURCE_DIR}/third_party)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/build)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/build)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/build)

# DPDK may be needed for different reasons (e.g., the transport is DPDK, or
# if the transport is not DPDK but the application needs DPDK libraries)
set(DPDK_NEEDED "true") # setting this because we need DPDK

# Options exposed to the user [eRPC CMake - skipped]
# Profile-guided optimization [eRPC CMake - skipped]
# Logging level [eRPC CMake - skipped]
# Testing for packet loss, machine failure, etc [eRPC CMake - skipped]
# SOURCES at line 118 [eRPC CMake - skipped]
# Transport-specific. Mellanox OFED drivers are the best choice for raw and
# infiniband, but they do not play well with DPDK. So we compile only one
# transport. Other transports are exluded using preprocessor macros. [line 152-205] - [eRPC CMake - skipped]
# Generate config.h [line 207-209] - [eRPC CMake - skipped]
# MICA sources - [eRPC CMake - skipped]
# The app to compile. Only one app is compiled to reduce compile time. [216-223] - [eRPC CMake - skipped]
# Add app-specific defines now, isolating them from the library and tests [225-270] - [eRPC CMake - skipped]

if(DPDK_NEEDED STREQUAL "true")
  message(STATUS "DPDK needed to build eRPC")

  if (WIN32)
    message(STATUS "DPDK for Windows")

    # Set this to your DPDK directory
    set(DPDK_WINDIR "${CMAKE_CURRENT_SOURCE_DIR}/../dpdk")
    if(NOT EXISTS "${DPDK_WINDIR}/lib/eal/include/rte_common.h")      # [exists in the DPDK directory]
      message(FATAL_ERROR "rte_common.h not found. DPDK_WINDIR = ${DPDK_WINLIBS}")
      return()
    endif()

    set(DPDK_WINLIBS
      acl bbdev bitratestats bpf cfgfile cmdline compressdev cryptodev
      distributor eal efd ethdev eventdev fib flow_classify graph gro gso hash
      ip_frag ipsec jobstats kni kvargs latencystats lpm mbuf member mempool
      meter metrics net node pci pdump pipeline port power rawdev rcu regexdev
      reorder rib ring sched security stack table telemetry timer vhost)
    foreach(lib_name IN LISTS DPDK_WINLIBS)
      include_directories(SYSTEM "${DPDK_WINDIR}/lib/${lib_name}")
    endforeach()

    include_directories(SYSTEM "${DPDK_WINDIR}/lib/eal/include")
    include_directories(SYSTEM "${DPDK_WINDIR}/lib/eal/windows/include")
    include_directories(SYSTEM "${DPDK_WINDIR}/lib/eal/x86/include")
    include_directories(SYSTEM "${DPDK_WINDIR}/build")
    include_directories(SYSTEM "${DPDK_WINDIR}/config")
    

    link_directories("${DPDK_WINDIR}/build/lib")
    link_directories("${DPDK_WINDIR}/build/drivers")

    set(LIBRARIES ${LIBRARIES}
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_cfgfile.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_hash.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_cmdline.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_pci.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_ethdev.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_meter.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_net.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_mbuf.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_mempool.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_rcu.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_ring.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_eal.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_telemetry.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/lib/librte_kvargs.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/drivers/librte_common_iavf.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/drivers/librte_bus_pci.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/drivers/librte_bus_vdev.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/drivers/librte_common_mlx5.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/drivers/librte_mempool_ring.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/drivers/librte_net_i40e.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/drivers/librte_net_ice.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/drivers/librte_net_mlx5.a"
      "-Wl,/WHOLEARCHIVE:${DPDK_WINDIR}/build/drivers/librte_net_vmxnet3.a"
      "-lws2_32" "-lmincore" "-ladvapi32" "-lsetupapi" "-ldbghelp"
      "C:/Program Files/Mellanox/MLNX_WinOF2_DevX_SDK/lib/mlx5devx.lib"
      "-Wl,/SUBSYSTEM:CONSOLE" "-lkernel32" "-luser32" "-lgdi32" "-lwinspool"
      "-lshell32" "-lole32" "-loleaut32" "-luuid" "-lcomdlg32" "-liphlpapi")

      set(ENV{RTE_SDK} ${CMAKE_SOURCE_DIR}/../dpdk)
      set(ENV{RTE_TARGET} ${CMAKE_SOURCE_DIR}/../dpdk/build)


get_directory_property(dirs INCLUDE_DIRECTORIES)
message("___________printing Include directories: ${dirs}")
      
  # else(WIN32)       # will never get into this block [line 333 on eRPC CMake]
  #   # Linux
  #   if(NOT DEFINED ENV{RTE_SDK})
  #     message(STATUS "RTE_SDK is not defined, try to use pkg-config")
  #     find_package(PkgConfig REQUIRED)
  #     pkg_check_modules(PKG_DPDK_LIB IMPORTED_TARGET libdpdk)
  #     if(NOT PKG_DPDK_LIB_FOUND) 
  #       message(FATAL_ERROR "DPDK library not found")
  #     endif()
  #     set(LIBRARIES ${LIBRARIES} "-Wl,--whole-archive -Wl,--as-needed" PkgConfig::PKG_DPDK_LIB pthread "-Wl,--no-whole-archive")
  #     include_directories(SYSTEM ${PKG_DPDK_LIB_INCLUDE_DIRS})
  #   else()
  #     set(RTE_SDK $ENV{RTE_SDK})
  #     set(DPDK_INSTALL_DIR "${RTE_SDK}/build/install/usr/local")

  #     # DPDK installs libraries into local/lib on Mariner, but
  #     # local/lib/x86_64-linux-gnu on Ubuntu
  #     set(DPDK_LIB_DIR "${DPDK_INSTALL_DIR}/lib/x86_64-linux-gnu")
  #     if(NOT EXISTS ${DPDK_LIB_DIR})
  #       set(DPDK_LIB_DIR "${DPDK_INSTALL_DIR}/lib")
  #       if(NOT EXISTS ${DPDK_LIB_DIR})
  #         message(FATAL_ERROR "${DPDK_LIB_DIR} not found. Did you run `ninja install`?")
  #       endif()
  #     endif()
  
  #     # We use some hacky command-line and CMake magic to construct DPDK library list
  #     execute_process(
  #       COMMAND bash -c "PKG_CONFIG_PATH=${DPDK_LIB_DIR}/pkgconfig pkg-config --static --libs-only-l libdpdk"
  #       OUTPUT_VARIABLE DPDK_PKGCONFIG_OUT RESULT_VARIABLE pkgconfig_ret)
  #     if(pkgconfig_ret EQUAL "1")
  #       message( FATAL_ERROR "Failed to run pkgconfig on DPDK (in ${DPDK_LIB_DIR}/pkgconfig). See error above.")
  #     endif()
  
  #     string(STRIP ${DPDK_PKGCONFIG_OUT} DPDK_PKGCONFIG_OUT) # Remove newline from pkg-config output
  #     set(LIBDPDK_LIBRARIES
  #       "-Wl,--whole-archive -Wl,--as-needed -L${DPDK_LIB_DIR} ${DPDK_PKGCONFIG_OUT} -lpthread -Wl,--no-whole-archive")
  #     set(LIBRARIES ${LIBRARIES} ${LIBDPDK_LIBRARIES})
  
  #     link_directories(${DPDK_LIB_DIR})
  #     include_directories(SYSTEM ${DPDK_INSTALL_DIR}/include)      
  #   endif()
  endif(WIN32)
else()
  message(STATUS "DPDK not needed to build machnet")
endif()

set(LIBRARIES ${LIBRARIES} gflags hdr_histogram_static)

# Compile the library [384-385] [eRPC CMake - skipped]

# Using link-time optimization sometimes requires building with sources instead
# of liberpc. See the hello world example's Makefile for an example of
# compiling with liberpc. [398-401] [eRPC CMake - skipped]

# Compile hello_world apps [408-412] [eRPC CMake - skipped]
# Compile the tests [414-419] [eRPC CMake - skipped]

set(BUILD_TESTING ON) # Not sure if CTest is getting invoked, did not find any add_test() calls
add_subdirectory(src)

# _________________________ machnet linux CMake file content ____________________

# find_package(gflags REQUIRED)
# set(GFLAGS_USE_TARGET_NAMESPACE ON)
# find_package(gflags CONFIG REQUIRED)
# target_link_libraries(main PRIVATE gflags::gflags)

#add_subdirectory(src)
