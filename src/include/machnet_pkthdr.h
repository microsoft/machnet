/**
 * @file machnet_pkthdr.h
 * @brief Description of the Machnet packet header.
 */

#ifndef SRC_INCLUDE_MACHNET_PKTHDR_H_
#define SRC_INCLUDE_MACHNET_PKTHDR_H_

#include <types.h>
#include <utils.h>

namespace juggler {
namespace net {

/**
 * Machnet Packet Header.
 */
struct __attribute__((packed)) MachnetPktHdr {
  static constexpr uint16_t kMagic = 0x4e53;
  be16_t magic;  // Magic value tagged after initialization for the flow.
  struct __attribute__((packed)) Port {
    static const uint8_t kSize = sizeof(be16_t);
    Port() = default;
    Port(uint16_t machnet_port) { port = be16_t(machnet_port); }
    bool operator==(const Port &rhs) const { return port == rhs.port; }
    bool operator==(be16_t rhs) const { return rhs == port; }
    bool operator!=(const Port &rhs) const { return port != rhs.port; }
    bool operator!=(be16_t rhs) const { return rhs != port; }
    be16_t port;
  };
  Port src_port;  // Source port.
  Port dst_port;  // Destination port.
  enum class MachnetFlags : uint8_t {
    kData = 0b0,
    kSyn = 0b1,         // SYN packet.
    kAck = 0b10,        // ACK packet.
    kSynAck = 0b11,     // SYN-ACK packet.
    kRst = 0b10000000,  // RST packet.
  };
  MachnetFlags net_flags;  // Network flags.
  uint8_t msg_flags;       // Field to reflect the `MachnetMsgBuf_t' flags.
  be32_t seqno;  // Sequence number to denote the packet counter in the flow.
  be32_t ackno;  // Sequence number to denote the packet counter in the flow.
  be64_t sack_bitmap;        // Bitmap of the SACKs received.
  be16_t sack_bitmap_count;  // Length of the SACK bitmap [0-64].
  be64_t timestamp1;         // Timestamp of the packet before sending.
};
static_assert(sizeof(MachnetPktHdr) == 34, "MachnetPktHdr size mismatch");

inline MachnetPktHdr::MachnetFlags operator|(MachnetPktHdr::MachnetFlags lhs,
                                             MachnetPktHdr::MachnetFlags rhs) {
  using MachnetFlagsType =
      std::underlying_type<MachnetPktHdr::MachnetFlags>::type;
  return MachnetPktHdr::MachnetFlags(static_cast<MachnetFlagsType>(lhs) |
                                     static_cast<MachnetFlagsType>(rhs));
}

inline MachnetPktHdr::MachnetFlags operator&(MachnetPktHdr::MachnetFlags lhs,
                                             MachnetPktHdr::MachnetFlags rhs) {
  using MachnetFlagsType =
      std::underlying_type<MachnetPktHdr::MachnetFlags>::type;
  return MachnetPktHdr::MachnetFlags(static_cast<MachnetFlagsType>(lhs) &
                                     static_cast<MachnetFlagsType>(rhs));
}

}  // namespace net
}  // namespace juggler

namespace std {
template <>
struct hash<juggler::net::MachnetPktHdr::Port> {
  std::size_t operator()(const juggler::net::MachnetPktHdr::Port &port) const {
    return juggler::utils::hash<uint32_t>(
        reinterpret_cast<const char *>(&port.port),
        sizeof(port.port.raw_value()));
  }
};
}  // namespace std

#endif  // SRC_INCLUDE_MACHNET_PKTHDR_H_
