/**
 * @file icmp.h
 * @brief ICMP header definition.
 */
#ifndef SRC_INCLUDE_ICMP_H_
#define SRC_INCLUDE_ICMP_H_
#include <types.h>
#include <utils.h>

namespace juggler {
namespace net {
struct __attribute__((packed)) Icmp {
  enum Type : uint8_t {
    kEchoReply = 0,
    kEchoRequest = 8,
  };
  Type type;
  static const uint8_t kCodeZero = 0;
  uint8_t code;
  uint16_t cksum;
  be16_t id;
  be16_t seq;
};

}  // namespace net
}  // namespace juggler

#endif  // SRC_INCLUDE_ICMP_H_
