/**
 * @file tcp.h
 * @brief TCP header definition for wire-format parsing/construction.
 *
 * This mirrors the structure of udp.h, providing a packed TCP header struct
 * with big-endian typed fields for use in the Machnet userspace networking
 * stack.
 */
#ifndef SRC_INCLUDE_TCP_H_
#define SRC_INCLUDE_TCP_H_

#include <types.h>
#include <udp.h>
#include <utils.h>

#include <cstdint>
#include <string>

namespace juggler {
namespace net {

/**
 * @struct Tcp
 * @brief Wire-format TCP header (20 bytes minimum, no options).
 */
struct __attribute__((packed)) Tcp {
  // Reuse Udp::Port for TCP ports — same 2-byte big-endian representation.
  using Port = Udp::Port;

  // TCP flag bits (in the flags byte).
  enum Flags : uint8_t {
    kFin = 0x01,
    kSyn = 0x02,
    kRst = 0x04,
    kPsh = 0x08,
    kAck = 0x10,
    kUrg = 0x20,
  };

  Port src_port;
  Port dst_port;
  be32_t seq_num;       ///< Sequence number.
  be32_t ack_num;       ///< Acknowledgement number.
  uint8_t data_offset;  ///< Upper 4 bits = data offset (in 32-bit words).
  uint8_t flags;        ///< TCP flags (lower 6 bits meaningful).
  be16_t window;        ///< Receive window size.
  uint16_t checksum;    ///< TCP checksum (or 0 if offloaded).
  be16_t urgent_ptr;    ///< Urgent pointer.

  /// @brief Returns the header length in bytes (data_offset field × 4).
  uint8_t header_length() const { return (data_offset >> 4) * 4; }

  /// @brief Sets the data offset field for a given header length in bytes.
  /// @param len Header length in bytes (must be a multiple of 4, >= 20).
  void set_header_length(uint8_t len) {
    data_offset = static_cast<uint8_t>((len / 4) << 4);
  }

  std::string ToString() const;
};
static_assert(sizeof(Tcp) == 20, "TCP header must be 20 bytes");

}  // namespace net
}  // namespace juggler

#endif  // SRC_INCLUDE_TCP_H_
