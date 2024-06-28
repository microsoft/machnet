#ifndef SRC_INCLUDE_TTIME_H_
#define SRC_INCLUDE_TTIME_H_

#include <rte_cycles.h>
#define SRC_DPDK_TIMER 1

#include <glog/logging.h>
#include <rte_timer.h>
#include <stdio.h>
#include <time.h>

#include <concepts>

namespace juggler {

namespace time {

// TODO(ilias): Reconsider whether this should be global for all threads. Which
// CPUs have non-synchronized TSC these days?
extern thread_local uint64_t tsc_hz;

static inline uint64_t rdtsc() { return rte_rdtsc(); }

[[maybe_unused]] static inline uint64_t estimate_tsc_hz() {
  return rte_get_tsc_hz();
}

template <typename T = uint64_t>
requires std::integral<T> || std::floating_point<T>
[[maybe_unused]] static inline T cycles_to_ns(uint64_t cycles) {
  DCHECK_NE(tsc_hz, 0);

  return static_cast<T>(cycles * 1E9 / tsc_hz);
}

template <typename T = uint64_t>
requires std::integral<T> || std::floating_point<T>
[[maybe_unused]] static constexpr inline T cycles_to_us(uint64_t cycles) {
  return cycles_to_ns<T>(cycles) / 1E3;
}

template <typename T = double>
requires std::integral<T> || std::floating_point<T>
[[maybe_unused]] static constexpr inline T cycles_to_ms(uint64_t cycles) {
  return cycles_to_ns<T>(cycles) / 1E6;
}

template <typename T = double>
requires std::integral<T> || std::floating_point<T>
[[maybe_unused]] static constexpr inline T cycles_to_s(uint64_t cycles) {
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
