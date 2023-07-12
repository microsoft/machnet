/**
 * @file nsaas_pkthdr.h
 * @brief Description of the NSaaS packet header.
 */

#ifndef SRC_INCLUDE_NSAAS_PKTHDR_H_
#define SRC_INCLUDE_NSAAS_PKTHDR_H_

#include <types.h>

namespace juggler {
namespace net {

/**
 * NSaaS Packet Header.
 */
struct __attribute__((packed)) NSaaSPktHdr {
  static constexpr uint16_t kMagic = 0x4e53;
  be16_t magic;  // Magic value tagged after initialization for the flow.
  enum class NSaaSNetFlags : uint8_t {
    kData = 0b0,
    kSyn = 0b1,         // SYN packet.
    kAck = 0b10,        // ACK packet.
    kSynAck = 0b11,     // SYN-ACK packet.
    kRst = 0b10000000,  // RST packet.
  };
  NSaaSNetFlags net_flags;  // Network flags.
  uint8_t msg_flags;        // Field to reflect the `NSaaSMsgBuf_t' flags.
  be32_t seqno;  // Sequence number to denote the packet counter in the flow.
  be32_t ackno;  // Sequence number to denote the packet counter in the flow.
  be64_t sack_bitmap;        // Bitmap of the SACKs received.
  be16_t sack_bitmap_count;  // Length of the SACK bitmap [0-64].
  be64_t timestamp1;         // Timestamp of the packet before sending.
};
static_assert(sizeof(NSaaSPktHdr) == 30, "NSaaSPktHdr size mismatch");

inline NSaaSPktHdr::NSaaSNetFlags operator|(NSaaSPktHdr::NSaaSNetFlags lhs,
                                            NSaaSPktHdr::NSaaSNetFlags rhs) {
  using NSaaSNetFlagsType =
      std::underlying_type<NSaaSPktHdr::NSaaSNetFlags>::type;
  return NSaaSPktHdr::NSaaSNetFlags(static_cast<NSaaSNetFlagsType>(lhs) |
                                    static_cast<NSaaSNetFlagsType>(rhs));
}

inline NSaaSPktHdr::NSaaSNetFlags operator&(NSaaSPktHdr::NSaaSNetFlags lhs,
                                            NSaaSPktHdr::NSaaSNetFlags rhs) {
  using NSaaSNetFlagsType =
      std::underlying_type<NSaaSPktHdr::NSaaSNetFlags>::type;
  return NSaaSPktHdr::NSaaSNetFlags(static_cast<NSaaSNetFlagsType>(lhs) &
                                    static_cast<NSaaSNetFlagsType>(rhs));
}

}  // namespace net
}  // namespace juggler

#endif  // SRC_INCLUDE_NSAAS_PKTHDR_H_
