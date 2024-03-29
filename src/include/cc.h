/**
 * @file cc.h
 * This file contains Congestion Control related definitions.
 */
#ifndef SRC_INCLUDE_CC_H_
#define SRC_INCLUDE_CC_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "utils.h"

namespace juggler {
namespace net {
namespace swift {

constexpr bool seqno_lt(uint32_t a, uint32_t b) {
  return static_cast<int32_t>(a - b) < 0;
}
constexpr bool seqno_le(uint32_t a, uint32_t b) {
  return static_cast<int32_t>(a - b) <= 0;
}
constexpr bool seqno_eq(uint32_t a, uint32_t b) {
  return static_cast<int32_t>(a - b) == 0;
}
constexpr bool seqno_ge(uint32_t a, uint32_t b) {
  return static_cast<int32_t>(a - b) >= 0;
}
constexpr bool seqno_gt(uint32_t a, uint32_t b) {
  return static_cast<int32_t>(a - b) > 0;
}

/**
 * @brief Swift Congestion Control (SWCC) protocol control block.
 */
// TODO(ilias): First-cut implementation. Needs a lot of work.
struct Pcb {
  static constexpr std::size_t kInitialCwnd = 32;
  static constexpr std::size_t kSackBitmapSize = 256;
  static constexpr std::size_t kRexmitThreshold = 3;
  static constexpr int kRtoThresholdInTicks = 3;  // in slow timer ticks.
  static constexpr int kRtoDisabled = -1;
  Pcb() {}

  // Return the sender effective window in # of packets.
  uint32_t effective_wnd() const {
    uint32_t effective_wnd = cwnd - (snd_nxt - snd_una - snd_ooo_acks);
    return effective_wnd > cwnd ? 0 : effective_wnd;
  }

  uint32_t seqno() const { return snd_nxt; }
  uint32_t get_snd_nxt() {
    uint32_t seqno = snd_nxt;
    snd_nxt++;
    return seqno;
  }

  std::string ToString() const {
    std::string s;
    s += "[CC] snd_nxt: " + std::to_string(snd_nxt) +
         ", snd_una: " + std::to_string(snd_una) +
         ", rcv_nxt: " + std::to_string(rcv_nxt) +
         ", cwnd: " + std::to_string(cwnd) +
         ", fast_rexmits: " + std::to_string(fast_rexmits) +
         ", rto_rexmits: " + std::to_string(rto_rexmits) +
         ", effective_wnd: " + std::to_string(effective_wnd());
    return s;
  }

  uint32_t ackno() const { return rcv_nxt; }
  bool max_rexmits_reached() const { return rto_rexmits >= kRexmitThreshold; }
  bool rto_disabled() const { return rto_timer == kRtoDisabled; }
  bool rto_expired() const { return rto_timer >= kRtoThresholdInTicks; }

  uint32_t get_rcv_nxt() const { return rcv_nxt; }
  void advance_rcv_nxt() { rcv_nxt++; }
  void rto_enable() { rto_timer = 0; }
  void rto_disable() { rto_timer = kRtoDisabled; }
  void rto_reset() { rto_enable(); }
  void rto_maybe_reset() {
    if (snd_una == snd_nxt)
      rto_disable();
    else
      rto_reset();
  }
  void rto_advance() { rto_timer++; }

  void sack_bitmap_shift_right_one() {
    constexpr size_t sack_bitmap_bucket_max_idx =
        kSackBitmapSize / sizeof(sack_bitmap[0]) - 1;

    for (size_t i = sack_bitmap_bucket_max_idx; i > 0; --i) {
      // Shift the current each bucket to the right by 1 and take the most
      // significant bit from the previous bucket
      const uint64_t sack_bitmap_left_bucket = sack_bitmap[i - 1];
      uint64_t &sack_bitmap_right_bucket = sack_bitmap[i];

      sack_bitmap_right_bucket =
          (sack_bitmap_right_bucket >> 1) | (sack_bitmap_left_bucket << 63);
    }

    // Special handling for the left most bucket
    uint64_t &sack_bitmap_left_most_bucket = sack_bitmap[0];
    sack_bitmap_left_most_bucket >>= 1;

    sack_bitmap_count--;
  }

  void sack_bitmap_bit_set(const size_t index) {
    constexpr size_t sack_bitmap_bucket_size = sizeof(sack_bitmap[0]);
    const size_t sack_bitmap_bucket_idx = index / sack_bitmap_bucket_size;
    const size_t sack_bitmap_idx_in_bucket = index % sack_bitmap_bucket_size;

    LOG_IF(FATAL, index >= kSackBitmapSize) << "Index out of bounds: " << index;

    sack_bitmap[sack_bitmap_bucket_idx] |= (1ULL << sack_bitmap_idx_in_bucket);

    sack_bitmap_count++;
  }

  uint32_t target_delay{0};
  uint32_t snd_nxt{0};
  uint32_t snd_una{0};
  uint32_t snd_ooo_acks{0};
  uint32_t rcv_nxt{0};
  uint64_t sack_bitmap[kSackBitmapSize / sizeof(uint64_t)]{0};
  uint8_t sack_bitmap_count{0};
  uint16_t cwnd{kInitialCwnd};
  uint16_t duplicate_acks{0};
  int rto_timer{kRtoDisabled};
  uint16_t fast_rexmits{0};
  uint16_t rto_rexmits{0};
};

}  // namespace swift
}  // namespace net
}  // namespace juggler

#endif  //  SRC_INCLUDE_CC_H_
