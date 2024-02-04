/**
 * @file cc.h
 * This file contains Congestion Control related definitions.
 */
#ifndef SRC_INCLUDE_CC_H_
#define SRC_INCLUDE_CC_H_

#include <cstddef>
#include <cstdint>
#include <vector>
#include <cmath>

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
  static constexpr std::size_t kInitialCwnd = 1; // star from 1 as inital window
  static constexpr std::size_t kRexmitThreshold = 3;
  static constexpr int kRtoThresholdInTicks = 3;  // in slow timer ticks.
  static constexpr int kRtoDisabled = -1;
  Pcb() {}

  /**
   * @brief This function is called when we receive an ack from the receiver.
   * It will update the congestion window based on acked_packets,
   * and will also update the state of the congestion control.
   * param acked_packets: number of packets acked by the receiver.
   */
  void expand_window(uint16_t acked_packets) {
    if (state == 2) {
      // Congestion avoidance.
      if (cwnd == 0){
        LOG(ERROR) << "cwnd is zero";
        state = 1;
        cwnd = 1;
      } else {
        cwnd += 1.0*acked_packets/cwnd;
      }
    } else if(state == 1) {
      // Slow start. We make sure we don't overflow the window.
      if (cwnd + acked_packets > cwnd) {
        cwnd += acked_packets;
      }

      if (cwnd >= ssthresh) {
        // go to congestion avoidance
        state = 2;
      }
    } else {
      // first time we started the 
      cwnd += acked_packets;
    }


    // TODO: This should not be more than what channel can handle, also TX len might need to be checked

    // congestion window more than TX len does not make sense.
    if (cwnd > 256) {
      cwnd = 256;
      ssthresh = cwnd/2;
    }

  }

  void reduce_window() {
    ssthresh = cwnd / 2;
    cwnd = 1; // we can also set it to initial value
    state = 1; // slow start
  }
    
  // Return the sender effective window in # of packets.
  uint32_t effective_wnd() const {
    uint32_t effective_wnd = floor(cwnd) - (snd_nxt - snd_una - snd_ooo_acks);
    return effective_wnd > floor(cwnd) ? 0 : effective_wnd;
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
         ", effective_wnd: " + std::to_string(effective_wnd()) +
         ", state: " + std::to_string(state) +
         ", ss_thresh: " + std::to_string(ssthresh);
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
    // Since we have 4 elements in the SACK bitmap we start from 3
    for (int i = 3; i > 0; --i) {
        // Shift the current element to the right
        sack_bitmap_test[i] = (sack_bitmap_test[i] >> 1) | (sack_bitmap_test[i - 1] << 63);
    }
    
    // Special handling for the first element
    sack_bitmap_test[0] >>= 1;

    sack_bitmap_count--;
  
  }

  void sack_bitmap_setbit(uint32_t index) {
    if (index >= 256) {
      LOG(ERROR) << "Index out of bounds: " << index;
    }
    
    sack_bitmap_test[index/64] |= (1ULL << index % 64);

    sack_bitmap_count++;
 }

  uint32_t target_delay{0};
  uint32_t snd_nxt{0};
  uint32_t snd_una{0};
  uint32_t snd_ooo_acks{0};
  uint32_t rcv_nxt{0};
  uint64_t sack_bitmap{0};
  uint64_t sack_bitmap_test[4]{0};
  uint8_t sack_bitmap_count{0};
  float cwnd{kInitialCwnd};
  uint16_t duplicate_acks{0};
  int rto_timer{kRtoDisabled};
  uint16_t fast_rexmits{0};
  uint16_t rto_rexmits{0};
  uint16_t ssthresh{0}; // slow start threshold.
  uint8_t state{0}; // 0: just starting for the first time 1: slow start, 2: congestion avoidance.
};

}  // namespace swift
}  // namespace net
}  // namespace juggler

#endif  //  SRC_INCLUDE_CC_H_
