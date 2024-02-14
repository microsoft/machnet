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
  static constexpr std::size_t kInitialCwnd = 128;
  static constexpr std::size_t kSackBitmapSize = 265;
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

  void shift_right_sack_bitmap() {
    for (uint8_t i = kSackBitmapSize/64 - 1; i > 0; --i) {
        // Shift the current element to the right
        sack_bitmap[i] = (sack_bitmap[i] >> 1) | (sack_bitmap[i - 1] << 63);
    }
    
    // Special handling for the first element
    sack_bitmap[0] >>= 1;
    sack_bitmap_count--;
  
  }

  void sack_bitmap_setbit(uint32_t index) {
    if (index >= 256) {
      LOG(ERROR) << "Index out of bounds: " << index;
    }
    
    sack_bitmap[index/64] |= (1ULL << index % 64);

    sack_bitmap_count++;
 }

  uint32_t target_delay{0};
  uint32_t snd_nxt{0};
  uint32_t snd_una{0};
  uint32_t snd_ooo_acks{0};
  uint32_t rcv_nxt{0};
  uint64_t sack_bitmap[kSackBitmapSize/sizeof(uint64_t)]{0};
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
