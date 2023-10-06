include_directories(SYSTEM ${LIBDPDK_INCLUDE_DIRS})

# Build each test in the list as a separate executable
file(GLOB_RECURSE TEST_FILES "${PROJECT_SOURCE_DIR}/src/*_test.cc" )
# list(FILTER TEST_FILES EXCLUDE REGEX "${PROJECT_SOURCE_DIR}/src/tests/.*" )

foreach(test_name IN LISTS TEST_FILES)
  get_filename_component(test_bin ${test_name} NAME_WE)
  add_executable(${test_bin} ${test_name})
  target_link_libraries(${test_bin} PUBLIC
    core machnet_shim glog gtest gmock ${LIBDPDK_LIBRARIES} hugetlbfs rt)
  add_test(NAME ${test_bin} COMMAND ${test_bin})
endforeach()
