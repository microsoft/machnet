# Create a library called "core" which includes the source file "dpdk.cc".
# The extension is already found. Any number of sources could be listed here.
file(GLOB_RECURSE SRC_FILES ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)
list(FILTER SRC_FILES EXCLUDE REGEX "^.*_test\\.(cc|h)$")
list(FILTER SRC_FILES EXCLUDE REGEX "^.*channel_bench\\.(cc|h)$") # prevents channel_bench.obj duplication
list(FILTER SRC_FILES EXCLUDE REGEX "^.*driver\\shm\\.(cc|h)$") # ignoring shm

add_library (core STATIC ${SRC_FILES})
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

target_include_directories(core PUBLIC ../include)
target_include_directories(core PRIVATE .)
target_link_libraries(core PRIVATE)

if (WIN32)
    target_link_libraries(core PRIVATE nlohmann_json::nlohmann_json)
endif()

# link_directories($ENV{RTE_SDK}/$ENV{RTE_TARGET}/lib/)
# find_library(DPDK_LIB NAMES libdpdk.a dpdk)
# target_link_libraries(core PRIVATE dpdk numa dl)
# target_link_libraries(core PRIVATE -Wl,--whole-archive dpdk -Wl,--no-whole-archive numa dl ${IBVERBS} ${LIBMLX4} ${LIBMLX5})
