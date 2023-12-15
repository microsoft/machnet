/**
 * @file common.h
 * @brief Common hardcoded constants used in the project.
 */

#ifndef SRC_INCLUDE_COMMON_H_
#define SRC_INCLUDE_COMMON_H_

#include <new>

namespace juggler {
#ifdef __cpp_lib_hardware_interference_size
using std::hardware_constructive_interference_size;
using std::hardware_destructive_interference_size;
#else
// 64 bytes on x86-64 │ L1_CACHE_BYTES │ L1_CACHE_SHIFT │ __cacheline_aligned │
// ...
constexpr std::size_t hardware_constructive_interference_size = 64;
constexpr std::size_t hardware_destructive_interference_size = 64;
#endif
// TODO(ilias): Adding an assertion for now, to prevent incompatibilities
// with the C helper library.
static_assert(hardware_constructive_interference_size == 64);
static_assert(hardware_destructive_interference_size == 64);

// x86_64 Page size.
// TODO(ilias): Write an initialization function to get this
// programmatically using `sysconf'.
static const std::size_t kPageSize = 4096;

static const std::size_t kHugePage2MSize = 2 * 1024 * 1024;

static constexpr bool kShmZeroCopyEnabled = false;

enum class CopyMode {
  kMemCopy,
  kZeroCopy,
};

}  // namespace juggler

#endif  // SRC_INCLUDE_COMMON_H_
