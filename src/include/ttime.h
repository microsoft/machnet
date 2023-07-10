#ifndef SRC_INCLUDE_TTIME_H_
#define SRC_INCLUDE_TTIME_H_

#include <emmintrin.h>
#include <glog/logging.h>
#include <stdio.h>
#include <time.h>

#include <concepts>

namespace juggler {
namespace time {

// TODO(ilias): Reconsider whether this should be global for all threads. Which
// CPUs have non-synchronized TSC these days?
extern thread_local uint64_t tsc_hz;

static inline uint64_t rdtsc() {
  uint32_t hi, lo;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return (uint64_t)lo | ((uint64_t)hi << 32);
}

[[maybe_unused]] static inline uint64_t estimate_tsc_hz() {
  timespec start, end;
  uint64_t start_tsc, end_tsc;
  auto precise_tsc = []() {
    _mm_mfence();
    return rdtsc();
  };

  clock_gettime(CLOCK_MONOTONIC_RAW, &start);
  start_tsc = precise_tsc();
  for (auto i = 0; i <= 1E6; i++) precise_tsc();
  end_tsc = precise_tsc();
  clock_gettime(CLOCK_MONOTONIC_RAW, &end);

  auto ns_diff = [](timespec s, timespec e) {
    uint64_t ns = 0;

    ns = 1E9 * (e.tv_sec - s.tv_sec);
    ns += e.tv_nsec - s.tv_nsec;

    return ns;
  };

  uint64_t cycles = end_tsc - start_tsc;
  uint64_t time_in_ns = ns_diff(start, end);

  // Return Hz.
  return cycles * 1E9 / time_in_ns;
}

template <typename T = uint64_t>
    requires std::integral<T> ||
    std::floating_point<T>[[maybe_unused]] static inline T cycles_to_ns(
        uint64_t cycles) {
  DCHECK_NE(tsc_hz, 0);

  return static_cast<T>(cycles * 1E9 / tsc_hz);
}

template <typename T = uint64_t>
    requires std::integral<T> ||
    std::floating_point<T>[[maybe_unused]] static constexpr inline T
    cycles_to_us(uint64_t cycles) {
  return cycles_to_ns<T>(cycles) / 1E3;
}

template <typename T = double>
    requires std::integral<T> ||
    std::floating_point<T>[[maybe_unused]] static constexpr inline T
    cycles_to_ms(uint64_t cycles) {
  return cycles_to_ns<T>(cycles) / 1E6;
}

template <typename T = double>
    requires std::integral<T> ||
    std::floating_point<T>[[maybe_unused]] static constexpr inline T
    cycles_to_s(uint64_t cycles) {
  return cycles_to_ns<T>(cycles) / 1E9;
}

[[maybe_unused]] static inline uint64_t us_to_cycles(uint64_t us) {
  return us * tsc_hz / 1E6;
}

[[maybe_unused]] static inline uint64_t ms_to_cycles(uint64_t ms) {
  return ms * tsc_hz / 1E3;
}

[[maybe_unused]] static inline uint64_t s_to_cycles(uint64_t s) {
  return s * tsc_hz;
}

}  // namespace time
}  // namespace juggler

#endif  // SRC_INCLUDE_TTIME_H_
