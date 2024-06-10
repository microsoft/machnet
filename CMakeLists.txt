cmake_minimum_required(VERSION 3.0)
set(CMAKE_INSTALL_PREFIX "/home/rr-windows-vm/machnet-sumodule/build" CACHE PATH "Installation prefix")
project(juggler VERSION 0.0.1 DESCRIPTION "Packet Juggling framework in C++" LANGUAGES CXX C)
include(CMakeDependentOption)

set(CMAKE_CXX_STANDARD 11)
# set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
set(BUILD_TESTING OFF) # Disable testing for third-party modules

# Common sub-projects: gflags
add_subdirectory(third_party/gflags)
include_directories(SYSTEM ${CMAKE_BINARY_DIR}/third_party/gflags/include)

# Glog doesn't correctly define its include directories, so we need to specify manually
set(CMAKE_BUILD_TYPE Release)
add_definitions(-DGLOG_USE_GLOG_EXPORT)
add_subdirectory(third_party/glog)
include_directories(SYSTEM third_party/glog/src)
include_directories(SYSTEM ${CMAKE_BINARY_DIR}/third_party/glog)

# Common sub-projects: gtest
add_subdirectory(third_party/googletest)
include_directories(SYSTEM third_party/googletest/googlemock/include)

# Build google benchmark (target: benchmark) do not build tests of benchmarking lib
set(BENCHMARK_ENABLE_INSTALL OFF)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "Suppressing benchmark's tests" FORCE)
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(third_party/googlebench)
include_directories(SYSTEM third_party/googlebench/include)

# Common sub-projects: HdrHistogram
set(HDR_HISTOGRAM_BUILD_PROGRAMS OFF CACHE BOOL "Minimize HDR histogram build")
set(HDR_LOG_REQUIRED OFF CACHE BOOL "Disable HDR histogram's log to avoid zlib dependency")
add_subdirectory(third_party/HdrHistogram_c)
include_directories(SYSTEM third_party/HdrHistogram_c/include/)

include_directories(SYSTEM third_party/xxHash)

set(DPDK_NEEDED "true") # setting this because we need DPDK

if(DPDK_NEEDED STREQUAL "true")
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
    include_directories(SYSTEM "${DPDK_WINDIR}/drivers/bus/pci")
    

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
  endif(WIN32)
else()
  message(STATUS "DPDK not needed to build machnet")
endif()

set(BUILD_TESTING ON)
add_subdirectory(src)