# Create a library called "core" which includes the source file "dpdk.cc".
# The extension is already found. Any number of sources could be listed here.
file(GLOB_RECURSE SRC_FILES ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)
list(FILTER SRC_FILES EXCLUDE REGEX "^.*_test\\.(cc|h)$")
list(FILTER SRC_FILES EXCLUDE REGEX "^.*channel_bench\\.(cc|h)$")

# set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} MD")
add_library (core STATIC ${SRC_FILES})
# set_property(TARGET core PROPERTY
#       MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:DebugDLL")

set_target_properties(core PROPERTIES
           VERSION ${PROJECT_VERSION}
           SOVERSION 1
           PUBLIC_HEADER dpdk.h)

if (WIN32)
    target_include_directories(core SYSTEM PUBLIC $ENV{RTE_SDK}/lib/eal/include)
    # target_include_directories(core SYSTEM PUBLIC $ENV{RTE_SDK}/lib/eal/windows/include)
    target_include_directories(core SYSTEM PUBLIC $ENV{RTE_SDK}/lib/eal/x86/include)
    # target_include_directories(core SYSTEM PUBLIC $ENV{RTE_SDK}/build)
    # target_include_directories(core SYSTEM PUBLIC $ENV{RTE_SDK}/config)
    # target_include_directories(core SYSTEM PUBLIC $ENV{RTE_SDK}/drivers/bus/pci)
else()
    target_include_directories(core SYSTEM PUBLIC $ENV{RTE_SDK}/build/install/usr/local/include)
endif()

target_include_directories(core PUBLIC ../include) #linux
target_include_directories(core PRIVATE .)
target_link_libraries(core PRIVATE)

message(STATUS "flag 4")

if (WIN32)
    target_link_libraries(core PRIVATE nlohmann_json::nlohmann_json)
endif()

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


# link_directories($ENV{RTE_SDK}/$ENV{RTE_TARGET}/lib/)
# find_library(DPDK_LIB NAMES libdpdk.a dpdk)
# target_link_libraries(core PRIVATE dpdk numa dl)
# target_link_libraries(core PRIVATE -Wl,--whole-archive dpdk -Wl,--no-whole-archive numa dl ${IBVERBS} ${LIBMLX4} ${LIBMLX5})
