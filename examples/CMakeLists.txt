set(target_name hello_world)
include_directories(../src/ext)
include_directories(../src/ext/windows)
link_directories(../build/bin)
add_executable (${target_name} hello_world.cc)
# target_include_directories(
#     ${target_name} 
#     PUBLIC 
#     ${CMAKE_SOURCE_DIR}/src/ext
#     ${CMAKE_SOURCE_DIR}/src/ext/windows
# )

if (WIN32)
    target_link_libraries(${target_name} PUBLIC glog core machnet_shim gflags) # rt breaks the build
else ()
    target_link_libraries(${target_name} PUBLIC glog core machnet_shim rt ${LIBDPDK_LIBRARIES})
endif ()
