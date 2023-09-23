#ifndef SRC_INCLUDE_UDP_H_
#define SRC_INCLUDE_UDP_H_

#include <types.h>
#include <utils.h>

namespace juggler {
namespace net {

struct __attribute__((packed)) Udp {
  struct __attribute__((packed)) Port {
    static const uint8_t kSize = 2;
    Port() = default;
    Port(uint16_t udp_port) { port = be16_t(udp_port); }
    bool operator==(const Port &rhs) const { return port == rhs.port; }
    bool operator==(be16_t rhs) const { return rhs == port; }
    bool operator!=(const Port &rhs) const { return port != rhs.port; }
    bool operator!=(be16_t rhs) const { return rhs != port; }

    be16_t port;
  };

  std::string ToString() const;

  Port src_port;
  Port dst_port;
  be16_t len;
  be16_t cksum;
};

}  // namespace net
}  // namespace juggler

namespace std {
template <>
struct hash<juggler::net::Udp::Port> {
  std::size_t operator()(const juggler::net::Udp::Port &port) const {
    return juggler::utils::hash<uint32_t>(
        reinterpret_cast<const char *>(&port.port),
        sizeof(port.port.raw_value()));
  }
};
}  // namespace std

#endif  // SRC_INCLUDE_UDP_H_
