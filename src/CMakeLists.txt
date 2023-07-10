set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Select flags.
set(CMAKE_C_FLAGS "-Wall -msse4.2")
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O3 -DNDEBUG -g")
set(CMAKE_C_FLAGS_DEBUG "-O0 -g -DDEBUG -fno-omit-frame-pointer -fsanitize=address")

set(CMAKE_CXX_FLAGS "-Wall -fno-rtti -fno-exceptions -msse4.2")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O3 -DNDEBUG -g")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -fno-omit-frame-pointer -fsanitize=address -DDEBUG")
set(CMAKE_LINKER_FLAGS_DEBUG "${CMAKE_LINKER_FLAGS_DEBUG} -fno-omit-frame-pointer -fsanitize=address")

# Include 'libdpdk'.
find_package(PkgConfig REQUIRED)

# cityhash.
if(NOT DEFINED ENV{CITYHASH_SDK})
  set(CITYHASH_SDK "$ENV{HOME}/cityhash")
  message("CITYHASH_SDK is not defined; setting to: ${CITYHASH_SDK}")
else()
  set(CITYHASH_SDK $ENV{CITYHASH_SDK})
endif()
if (NOT EXISTS ${CITYHASH_SDK})
  message(FATAL_ERROR "cityhash SDK not found at ${CITYHASH_SDK}")
endif()

# DPDK.
if(NOT DEFINED ENV{RTE_SDK})
  set(RTE_SDK "$ENV{HOME}/dpdk")
  message(STATUS "RTE_SDK is not defined; setting to: ${RTE_SDK}")
else()
  set(RTE_SDK $ENV{RTE_SDK})
endif()

set(DPDK_INSTALL_DIR "${RTE_SDK}/build/install/usr/local")

# DPDK installs libraries into local/lib on Mariner, but
# local/lib/x86_64-linux-gnu on Ubuntu
set(DPDK_LIB_DIR "${DPDK_INSTALL_DIR}/lib/x86_64-linux-gnu")
if(NOT EXISTS ${DPDK_LIB_DIR})
  set(DPDK_LIB_DIR "${DPDK_INSTALL_DIR}/lib")
  if(NOT EXISTS ${DPDK_LIB_DIR})
    message(FATAL_ERROR "${DPDK_LIB_DIR} not found. Did you run `ninja install`?")
  endif()
endif()

# We use some hacky command-line and CMake magic to construct DPDK library list
execute_process(
  COMMAND bash -c "PKG_CONFIG_PATH=${DPDK_LIB_DIR}/pkgconfig pkg-config --static --libs-only-l libdpdk"
  OUTPUT_VARIABLE DPDK_PKGCONFIG_OUT RESULT_VARIABLE pkgconfig_ret)
if(pkgconfig_ret EQUAL "1")
  message( FATAL_ERROR "Failed to run pkgconfig on DPDK (in ${DPDK_LIB_DIR}/pkgconfig). See error above.")
endif()

string(STRIP ${DPDK_PKGCONFIG_OUT} DPDK_PKGCONFIG_OUT) # Remove newline from pkg-config output

set(LIBDPDK_LIBRARIES
  "-Wl,--whole-archive -Wl,--as-needed -L${DPDK_LIB_DIR} ${DPDK_PKGCONFIG_OUT} -lpthread -Wl,--no-whole-archive")

link_directories(${DPDK_LIB_DIR} ${USOCKETS_SDK} ${CITYHASH_SDK}/src/.libs)
include_directories(SYSTEM ${DPDK_INSTALL_DIR}/include ${CITYHASH_SDK}/src)

include_directories(include)
include_directories(ext)

add_subdirectory(core)
add_subdirectory(ext)
add_subdirectory(apps)

enable_testing()
add_subdirectory (tests)
