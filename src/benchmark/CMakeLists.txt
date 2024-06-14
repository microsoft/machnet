file(GLOB_RECURSE BENCHMARK_FILES "${PROJECT_SOURCE_DIR}/src/*_bench.cc" )
# set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded$<$<CONFIG:Debug>:DebugDLL>)

foreach(bench_name IN LISTS BENCHMARK_FILES)
  get_filename_component(bench_bin ${bench_name} NAME_WE)
  # set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} MD")
  add_executable(${bench_bin} ${bench_name})
  # set_property(TARGET ${bench_bin} PROPERTY
  #     MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:DebugDLL")
  # set_target_properties(${bench_bin} PROPERTIES
  #       CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL
  #       )

  if (WIN32)
    target_link_libraries(${bench_bin} PUBLIC core)
    
    # set(MACHNET_SHIM_LIB_DIR "${CMAKE_BINARY_DIR}/src/ext")
    # set(MACHNET_SHIM_H_DIR "${CMAKE_SOURCE_DIR}/src/ext")
    # include_directories(${CMAKE_SOURCE_DIR}/src/)
    # target_include_directories(${bench_bin} PUBLIC ${MACHNET_SHIM_H_DIR})
    # target_link_directories(${bench_bin} PUBLIC ${MACHNET_SHIM_LIB_DIR})
    # message(STATUS "MACHNET_SHIM_H_DIR: ${MACHNET_SHIM_H_DIR}")
    # message(STATUS "MACHNET_SHIM_LIB_DIR: ${MACHNET_SHIM_LIB_DIR}")
    
    # target_link_libraries(${bench_bin} PUBLIC machnet_shim)
    
    # set(MACHNET_SHIM_LIB_DIR "${CMAKE_BINARY_DIR}/bin/")
    # set(MACHNET_SHIM_H_DIR "${CMAKE_SOURCE_DIR}/src/ext")
    # # include_directories(${CMAKE_SOURCE_DIR}/src/)
    # target_include_directories(${bench_bin} PUBLIC ${MACHNET_SHIM_H_DIR})
    # target_link_directories(${bench_bin} PUBLIC ${MACHNET_SHIM_LIB_DIR})
    # message(STATUS "MACHNET_SHIM_H_DIR: ${MACHNET_SHIM_H_DIR}")
    # message(STATUS "MACHNET_SHIM_LIB_DIR: ${MACHNET_SHIM_LIB_DIR}")
    
    target_link_libraries(${bench_bin} PUBLIC machnet_shim) 

    target_link_libraries(${bench_bin} PUBLIC glog gtest benchmark ${LIBDPDK_LIBRARIES}) # hugetlbfs rt breaks the build
  else ()
    target_link_libraries(${bench_bin} PUBLIC
          core machnet_shim glog gtest benchmark ${LIBDPDK_LIBRARIES} hugetlbfs rt)
  endif ()

  
endforeach()
