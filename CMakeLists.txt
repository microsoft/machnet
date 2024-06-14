cmake_minimum_required(VERSION 3.0)
if (WIN32)
  cmake_policy(SET CMP0091 NEW)
  # set(CMAKE_TOOLCHAIN_FILE "C:/cygwin64/home/rr-windows-vm/vcpkg/scripts/buildsystems/vcpkg.cmake")
  set(CMAKE_INSTALL_PREFIX "/home/rr-windows-vm/machnet-sumodule/build" CACHE PATH "Installation prefix")
endif()
project(juggler VERSION 0.0.1 DESCRIPTION "Packet Juggling framework in C++" LANGUAGES CXX C)
include(CMakeDependentOption)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin") # testing channel_bench linking error

if(NOT CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "Setting build type to 'Release' as none was specified.")
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
  # set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "" "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS YES CACHE BOOL "Export all symbols" FORCE)

include(CTest)
set(BUILD_TESTING OFF) # Disable testing for third-party modules
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Global flag to prevent add_library() to create shared libraries if on." FORCE)



# set(USER_CXX_FLAGS ${CMAKE_CXX_FLAGS})
# # Turn off warnings and errors for all warnings in dependencies
# separate_arguments(CXX_FLAGS_LIST NATIVE_COMMAND ${CMAKE_CXX_FLAGS})
# list(REMOVE_ITEM CXX_FLAGS_LIST /WX -Werror -Werror=pendantic -pedantic-errors)
# if(MSVC)
#   list(FILTER CXX_FLAGS_LIST EXCLUDE REGEX "/[Ww]([0-4]?)(all)?") # Remove MSVC warning flags
#   list(APPEND CXX_FLAGS_LIST /w)
# else()
#   list(FILTER CXX_FLAGS_LIST EXCLUDE REGEX "-W(all|extra|everything)") # Remove GCC/LLVM flags
#   list(APPEND CXX_FLAGS_LIST -w)
# endif()
# list(JOIN CXX_FLAGS_LIST " " CMAKE_CXX_FLAGS)


# Common sub-projects: gflags
add_subdirectory(third_party/gflags)
include_directories(SYSTEM ${CMAKE_BINARY_DIR}/third_party/gflags/include)

# Glog doesn't correctly define its include directories, so we need to specify manually
# set(CMAKE_BUILD_TYPE Release)
set(WITH_GFLAGS OFF)
set(WITH_GTEST OFF)
# set(BUILD_SHARED_LIBS OFF)
# add_definitions(-DGLOG_USE_GLOG_EXPORT)
# add_definitions(-DGLOG_USE_GLOG_NO_EXPORT)
# add_definitions(-DGLOG_STATIC)
# add_definitions(-DGOOGLE_GLOG_DLL_DECL)
add_subdirectory(third_party/glog)
include_directories(SYSTEM third_party/glog/src)
include_directories(SYSTEM ${CMAKE_BINARY_DIR}/third_party/glog)



# Common sub-projects: gtest
# set(INSTALL_GTEST OFF)
# set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
# set(BUILD_SHARED_LIBS OFF)
add_subdirectory(third_party/googletest)
include_directories(SYSTEM third_party/googletest/googlemock/include)

# unset(BUILD_SHARED_LIBS CACHE)

# Build google benchmark (target: benchmark) do not build tests of benchmarking lib
# set(BENCHMARK_ENABLE_INSTALL OFF)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "Suppressing benchmark's tests" FORCE)
add_definitions(-DBENCHMARK_STATIC_DEFINE)
# set(HAVE_STD_REGEX ON)
# set(RUN_HAVE_STD_REGEX 1)
# set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
# set(CMAKE_BUILD_TYPE "Release" CACHE BOOL "" FORCE)
# set(MSVC true)
# set(HAVE_STD_REGEX ON)
add_subdirectory(third_party/googlebench)
include_directories(SYSTEM third_party/googlebench/include)
# unset(HAVE_STD_REGEX)
# unset(RUN_HAVE_STD_REGEX)

# Common sub-projects: HdrHistogram
set(HDR_HISTOGRAM_BUILD_PROGRAMS OFF CACHE BOOL "Minimize HDR histogram build")
set(HDR_LOG_REQUIRED OFF CACHE BOOL "Disable HDR histogram's log to avoid zlib dependency")
set(HDR_HISTOGRAM_BUILD_SHARED OFF CACHE BOOL "Prevent shared lib")
set(HDR_HISTOGRAM_INSTALL_SHARED OFF CACHE BOOL "Prevent shared lib install")
add_subdirectory(third_party/HdrHistogram_c)
include_directories(SYSTEM third_party/HdrHistogram_c/include/)
# unset(MSVC)


include_directories(SYSTEM third_party/xxHash)


# message(STATUS "_________________________---------------__________________")
# message(STATUS "CMAKE_CXX_SIMULATE_ID: ${CMAKE_CXX_SIMULATE_ID}")
# message(STATUS "CMAKE_CXX_COMPILER_ID: ${CMAKE_CXX_COMPILER_ID}")
# # message(STATUS "CMAKE_C_FLAGS: ${CMAKE_C_FLAGS}")
# message(STATUS "CMAKE_C_FLAGS_RELEASE: ${CMAKE_C_FLAGS_RELEASE}")
# message(STATUS "CMAKE_CXX_FLAGS_RELEASE: ${CMAKE_CXX_FLAGS_RELEASE}")
# message(STATUS "MSVC: ${MSVC}")
# message(STATUS "_________________________---------------__________________")
# message(STATUS "CMAKE_C_FLAGS: ${CMAKE_C_FLAGS}")
# message(STATUS "CMAKE_CXX_FLAGS: ${CMAKE_CXX_FLAGS}")

# return()


if (WIN32)
  # Common sub-projects: nlohmann/json (using vcpkg)
  # find_package(nlohmann_json CONFIG REQUIRED)
  # set(nlohmann-json_IMPLICIT_CONVERSIONS OFF)

  set(JSON_BuildTests OFF CACHE INTERNAL "")
  add_subdirectory(third_party/nlohmann_json)
  include_directories(SYSTEM third_party/nlohmann_json/include)
endif()

# unset(BUILD_SHARED_LIBS CACHE)
# set(BUILD_SHARED_LIBS ON)
set(DPDK_NEEDED "true") # setting this because we need DPDK
# set(CMAKE_CXX_FLAGS ${USER_CXX_FLAGS}) # get back the flags

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

    set(LIBDPDK_LIBRARIES ${LIBDPDK_LIBRARIES}
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

# set(BUILD_TESTING ON)
add_subdirectory(src)