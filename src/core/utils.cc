#include <utils.h>

#include <algorithm>
#include <fstream>

namespace juggler {
namespace utils {

void TimeLog::DumpToFile(std::string file_name) {
  // Sort the timestamps array by putting the oldest event first.
  // We could use regular sorting here, but instead we rotate to protect against
  // wraparounds and duplicate timestamps.
  post_process();
  auto oldest_entry = index_ <= time_log_.size() ? 0 : (index_ + 1) & bitmask_;
  LOG(INFO) << "oldest entry: " << oldest_entry;
  std::rotate(time_log_.begin(), time_log_.begin() + oldest_entry,
              time_log_.end());

  std::ofstream out_file(file_name);
  for (const auto &elem : time_log_) {
    out_file << elem << std::endl;
  }
}

template <typename T>
void TimeSeries<T>::DumpToFile(std::string file_name) {
  // Sort the timestamps array by putting the oldest event first.
  // We could use regular sorting here, but instead we rotate to protect against
  // wraparounds and duplicate timestamps.
  post_process();
  auto oldest_entry = index_ <= time_log_.size() ? 0 : (index_ + 1) & bitmask_;
  std::rotate(time_log_.begin(), time_log_.begin() + oldest_entry,
              time_log_.end());
  std::rotate(values_.begin(), values_.begin() + oldest_entry, values_.end());

  std::ofstream out_file(file_name);
  for (size_t i = 0; i <= bitmask_; ++i) {
    if (time_log_[i] == UINT64_MAX) break;
    out_file << time_log_[i] << "," << values_[i] << std::endl;
  }
}

}  // namespace utils
}  // namespace juggler
