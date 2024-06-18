file(GLOB_RECURSE BENCHMARK_FILES "${PROJECT_SOURCE_DIR}/src/*_bench.cc" )

foreach(bench_name IN LISTS BENCHMARK_FILES)
  get_filename_component(bench_bin ${bench_name} NAME_WE)
  add_executable(${bench_bin} ${bench_name})

  if (WIN32)
    target_link_libraries(${bench_bin} PUBLIC core machnet_shim glog gtest benchmark ${LIBDPDK_LIBRARIES}) # hugetlbfs rt breaks the build
  else()
    target_link_libraries(${bench_bin} PUBLIC
          core machnet_shim glog gtest benchmark ${LIBDPDK_LIBRARIES} hugetlbfs rt)
  endif()

  
endforeach()
