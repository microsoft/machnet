/**
 * @file utils.h
 * @brief General utility functions and classes for the project.
 */

#ifndef SRC_INCLUDE_UTILS_H_
#define SRC_INCLUDE_UTILS_H_

#include <glog/logging.h>
#include <sched.h>
#include <sys/stat.h>

#include <chrono>
#include <cstdarg>
#include <cstring>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "ttime.h"

#define XXH_STATIC_LINKING_ONLY
#define XXH_INLINE_ALL
#include "xxhash.h"

namespace juggler {
namespace utils {

// Placeholder in case we want to experiment with memory copy more.
[[maybe_unused]] static inline void Copy(void *__restrict__ dest,
                                         const void *__restrict__ src,
                                         std::size_t nbytes) {
  std::memcpy(dest, src, nbytes);
}

[[maybe_unused]] static inline std::string HexDump(uint8_t *data, size_t len) {
  std::stringstream ss;
  for (size_t i = 0; i < len; i++) {
    ss << "0x" << std::setfill('0') << std::setw(2) << std::hex
       << static_cast<int>(data[i]) << ", ";
  }
  return ss.str();
}

[[maybe_unused]] static inline std::string HexDump(const std::string &data) {
  return HexDump(reinterpret_cast<uint8_t *>(const_cast<char *>(data.data())),
                 data.size());
}

[[maybe_unused]] static inline std::string HexDump(
    const std::vector<uint8_t> &data) {
  return HexDump(const_cast<uint8_t *>(data.data()), data.size());
}

[[maybe_unused]] static inline std::string HexDump(
    const std::vector<char> &data) {
  return HexDump(reinterpret_cast<uint8_t *>(const_cast<char *>(data.data())),
                 data.size());
}

[[maybe_unused]] static inline std::string HexDump(
    const std::vector<int> &data) {
  return HexDump(reinterpret_cast<uint8_t *>(const_cast<int *>(data.data())),
                 data.size() * sizeof(int));
}

[[maybe_unused]] static inline std::string HexDump(
    const std::vector<uint32_t> &data) {
  return HexDump(
      reinterpret_cast<uint8_t *>(const_cast<uint32_t *>(data.data())),
      data.size() * sizeof(uint32_t));
}

[[maybe_unused]] static inline std::string HexDump(
    const std::vector<uint64_t> &data) {
  return HexDump(
      reinterpret_cast<uint8_t *>(const_cast<uint64_t *>(data.data())),
      data.size() * sizeof(uint64_t));
}

[[maybe_unused]] static inline std::string HexDump(
    const std::vector<std::string> &data) {
  std::stringstream ss;
  for (const auto &s : data) {
    ss << HexDump(s);
  }
  return ss.str();
}

[[maybe_unused]] static inline std::string HexDump(
    const std::vector<std::vector<uint8_t>> &data) {
  std::stringstream ss;
  for (const auto &s : data) {
    ss << HexDump(s);
  }
  return ss.str();
}

// SplitString implementation.
[[maybe_unused]] static inline std::vector<std::string> SplitString(
    const std::string &s, char delim) {
  std::vector<std::string> elems;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, delim)) {
    elems.push_back(item);
  }
  return elems;
}

[[maybe_unused]] static inline uint32_t toeplitz_hash(
    const std::array<uint32_t, 4> &tuple, const std::vector<uint8_t> &rss_key) {
  CHECK_EQ(rss_key.size(), 40);

  uint32_t hash = 0;
  uint32_t key_index = 0;
  uint32_t bit_index = 0;

  for (int i = 0; i < 104; ++i) {
    if (rss_key[key_index] & (1 << bit_index)) {
      hash ^= tuple[i / 32] & (1 << (31 - i % 32));
    }

    if (++bit_index == 8) {
      bit_index = 0;
      ++key_index;
    }
  }

  return hash;
}

/**
 * @brief Compute a 16-bit checksum from the input data.
 *
 * This function calculates a 16-bit checksum over a given input data using the
 * one's complement method. The checksum is computed by adding 16-bit words of
 * the input data. If the input data length is not a multiple of 2 bytes (16
 * bits), the last remaining byte is padded with zeros on its least significant
 * byte and included in the checksum. The sum is then folded into 16 bits by
 * adding the high and low parts, and finally, the bitwise complement of the sum
 * is returned.
 *
 * @param data Pointer to the input data. The input data is treated as a
 * sequence of 16-bit words.
 * @param length The length, in bytes, of the input data. This value should be
 * equal to the number of bytes in the array pointed to by the 'data' parameter.
 *
 * @return The 16-bit checksum of the input data.
 *
 * @warning This function assumes that the 'data' pointer is valid and that it
 * points to a byte array of at least 'length' bytes. Providing an invalid
 * pointer or incorrect length can result in undefined behavior.
 */
[[maybe_unused]] static inline uint16_t ComputeChecksum16(const uint8_t *data,
                                                          size_t length) {
  uint32_t sum = 0;
  const uint16_t *ptr = reinterpret_cast<const uint16_t *>(data);

  for (; length > 1; length -= 2) {
    sum += *ptr++;
  }

  if (length == 1) {
    sum += static_cast<uint16_t>(*reinterpret_cast<const uint8_t *>(ptr) << 8);
  }

  while (sum >> 16) {
    sum = (sum & 0xFFFF) + (sum >> 16);
  }

  return static_cast<uint16_t>(~sum);
}

[[maybe_unused]] static inline void UUIDUnparse(const unsigned char uuid[16],
                                                char uuid_str[37]) {
  snprintf(
      uuid_str, 37,
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
      uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14],
      uuid[15]);
}

[[maybe_unused]] static inline std::string UUIDToString(
    const unsigned char uuid[16]) {
  char uuid_str[37] = {};
  UUIDUnparse(uuid, uuid_str);
  return std::string(uuid_str);
}

[[maybe_unused]] static bool BindThisThreadToCore(uint8_t core) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);       // Clear all CPUs
  CPU_SET(core, &cpuset);  // Set the requested core

  pthread_t current_thread = pthread_self();
  if (pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset) != 0) {
    perror("Could not set thread to specified core");
    return false;
  }
  return true;
}

[[maybe_unused]] static void SetHighPriorityAndSchedFifoForProcess() {
  struct sched_param sched_param;
  sched_param.sched_priority = 95;  // Use a high priority value
  int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sched_param);
  CHECK_EQ(rc, 0) << "Failed to set process priority and scheduling policy.";
}

[[maybe_unused]] static bool IsProcessRunningWithSUID() {
  return getuid() == 0;
}

[[maybe_unused]] static inline constexpr size_t cpuset_to_sizet(
    cpu_set_t mask) {
  size_t ret = 0;
  for (size_t i = 0; i < CPU_SETSIZE; ++i) {
    if (CPU_ISSET(i, &mask)) {
      ret |= (1ULL << i);
    }
  }
  return ret;
}

[[maybe_unused]] static inline constexpr cpu_set_t calculate_cpu_mask(
    std::size_t mask) {
  cpu_set_t m;
  // Zero the cpu set. CPU_ZERO uses memset which makes constexpr unhappy.
  CPU_XOR(&m, &m, &m);
  const auto mask_bitsize =
      std::min(sizeof(mask) * 8, static_cast<size_t>(CPU_SETSIZE));
  for (std::size_t i = 0; i < mask_bitsize; i++) {
    if (mask & (1ULL << i)) CPU_SET(i, &m);
  }
  return m;
}

template <typename T>
requires std::integral<T>
static constexpr inline bool is_power_of_two(T x) {
  return x && ((x & T(x - 1)) == 0);
}

// Align a size to a particular alignment boundary.
template <typename T>
requires std::integral<T>
static constexpr inline T align_size(T requested_size, T boundary) {
  T nchunks = (requested_size + boundary - 1) / boundary;
  return nchunks * boundary;
}

template <typename T>
static constexpr inline T hash(const char *str, size_t len) {
  constexpr size_t kXXHashSeed = 0xdeadbeef;
  if constexpr (std::is_same_v<T, uint32_t>) {
    return XXH32(str, len, kXXHashSeed);
  } else if constexpr (std::is_same_v<T, uint64_t>) {
    return XXH64(str, len, kXXHashSeed);
  } else {
    []<bool flag = false>() { static_assert(flag, "Unsupported hash type"); }
    ();
  }
}

// Derived from BESS.
class CmdLineOpts {
 public:
  explicit CmdLineOpts(std::initializer_list<std::string> args)
      : argc_(), argv_() {
    Append(args);
  }

  explicit CmdLineOpts(std::vector<std::string> args) : argc_(), argv_() {
    Append(args);
  }

  // Contructor to operate directly on command line arguments.
  CmdLineOpts(const int argc, const char *argv[]) : argc_(), argv_() {
    // We need the whole list of arguments, except from the first word which is
    // the basename.
    std::vector<std::string> args(&argv[1], &argv[1] - 1 + argc);
    Append(args);
  }

  explicit CmdLineOpts(std::string args) : argc_(), argv_() {
    std::istringstream iss(args);
    std::vector<std::string> arg_list;

    do {
      std::string arg;
      iss >> arg;

      auto trim = [](std::string &s) {
        // Trim from left.
        s.erase(s.begin(),
                std::find_if(s.begin(), s.end(), [](unsigned char ch) {
                  return !std::isspace(ch);
                }));

        // Trim from right.
        s.erase(std::find_if(s.rbegin(), s.rend(),
                             [](unsigned char ch) { return !std::isspace(ch); })
                    .base(),
                s.end());
      };
      trim(arg);
      if (arg.empty()) continue;

      arg_list.push_back(arg);
    } while (iss);

    Append(arg_list);
  }

  CmdLineOpts(CmdLineOpts const &) = default;

  void Append(std::vector<std::string> args) {
    for (const auto &arg : args) {
      argc_.emplace_back(arg.begin(), arg.end());
      argc_.back().emplace_back('\0');  // NULL terminate.
      argv_.push_back(argc_.back().data());
    }
  }

  std::string ToString() const {
    std::string ret;
    for (const auto &arg : argc_) {
      ret += arg.data();
      ret += " ";
    }
    return ret;
  }

  int GetArgc() const { return argc_.size(); }
  char **GetArgv() { return argv_.data(); }
  bool IsEmpty() const { return GetArgc() == 0; }

 private:
  std::vector<std::vector<char>> argc_;
  std::vector<char *> argv_;
};

static inline std::string FormatVarg(const char *fmt, va_list ap) {
  char *ptr = nullptr;
  int len = vasprintf(&ptr, fmt, ap);
  if (len < 0) return "<FormatVarg() error>";

  std::string ret(ptr, len);
  free(ptr);
  return ret;
}

[[maybe_unused]] static inline std::string Format(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const std::string s = FormatVarg(fmt, ap);
  va_end(ap);
  return s;
}

// Timestamp logging class.
// Instantiate a timestamp logging object by passing the maximum number of
// samples to store in the constructor.
// If more samples need to be stored, the underlying storage will be treated a
// circular buffer.
// If a timestamp is not explicitly passed in the `Record` methods, the TSC is
// being used as time source.
class TimeLog {
 public:
  static constexpr size_t kDefaultNumSamples = 16 * 4096;
  static_assert(is_power_of_two(kDefaultNumSamples));
  explicit TimeLog(size_t sample_size = kDefaultNumSamples)
      : bitmask_(sample_size - 1), time_log_(sample_size, 0), index_(0) {
    CHECK(is_power_of_two(kDefaultNumSamples))
        << "Sample size needs to be power of 2";
  }

  void Record() { Record(juggler::time::rdtsc()); }

  void Record(uint64_t timestamp) {
    time_log_[index_++] = timestamp;
    index_ &= bitmask_;
  }

  template <typename T>
  T Apply(std::function<T(std::vector<uint64_t> &)> func) {
    post_process();
    return func(time_log_);
  }

  void DumpToFile(std::string file_name);

 protected:
  void post_process() {
    if (time_log_.size() > index_) time_log_.resize(index_);
  }
  const size_t bitmask_;
  std::vector<uint64_t> time_log_;
  size_t index_;
};

// Timeseries logging class.
// Used to log a particular event type (templated), and the relevant timestamp
// that it occured.
template <typename T>
class TimeSeries : public TimeLog {
 public:
  explicit TimeSeries(size_t sample_size)
      : TimeLog(sample_size), values_(sample_size, T::value_type()) {}

  void Record(T value) {
    values_[index_] = value;
    TimeLog::Record();
  }

  void Record(uint64_t timestamp, T value) {
    values_[index_] = value;
    TimeLog::Record(timestamp);
  }

  template <typename K>
  K Apply(std::function<K(std::vector<uint64_t> &, std::vector<T> &)> func) {
    post_process();
    return func(time_log_, values_);
  }

  void DumpToFile(std::string file_name);

 private:
  void post_process() {
    TimeLog::post_process();
    if (values_.size() > index_) values_.resize(index_);
  }
  std::vector<T> values_;
};

/// Simple timer that uses std::chrono
class ChronoTimer {
 public:
  ChronoTimer() { Reset(); }
  void Reset() { start_time_ = std::chrono::high_resolution_clock::now(); }

  /// Return seconds elapsed since this timer was created or last reset
  double GetSeconds() const { return GetNanoseconds() / 1e9; }

  /// Return milliseconds elapsed since this timer was created or last reset
  double GetMilliseconds() const { return GetNanoseconds() / 1e6; }

  /// Return microseconds elapsed since this timer was created or last reset
  double GetMicroseconds() const { return GetNanoseconds() / 1e3; }

  /// Return nanoseconds elapsed since this timer was created or last reset
  size_t GetNanoseconds() const {
    return static_cast<size_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now() - start_time_)
            .count());
  }

 private:
  std::chrono::time_point<std::chrono::high_resolution_clock> start_time_;
};

}  // namespace utils
}  // namespace juggler

#endif  // SRC_INCLUDE_UTILS_H_
