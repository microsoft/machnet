#ifndef SRC_INCLUDE_IPV4_H_
#define SRC_INCLUDE_IPV4_H_

#include <arpa/inet.h>
#include <types.h>
#include <utils.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace juggler {
namespace net {

struct __attribute__((packed)) Ipv4 {
  static const uint8_t kDefaultTTL = 64;
  struct __attribute__((packed)) Address {
    static const uint8_t kSize = 4;
    Address() = default;
    Address(const uint8_t *addr) {
      juggler::utils::Copy(&address, addr, sizeof(address));
    }
    Address(uint32_t addr) { address = be32_t(addr); }

    static bool IsValid(const std::string &addr);

    /// If addr is not a valid IPv4 address, return a zero-valued IP address
    static std::optional<Address> MakeAddress(const std::string &addr);

    Address &operator=(const Address &rhs) {
      address = rhs.address;
      return *this;
    }
    bool operator==(const Address &rhs) const { return address == rhs.address; }
    bool operator!=(const Address &rhs) const { return address != rhs.address; }
    bool operator==(const be32_t &rhs) const { return rhs == address; }
    bool operator!=(const be32_t &rhs) const { return rhs != address; }
    bool operator!=(be32_t rhs) const { return rhs != address; }
    bool operator==(const uint32_t &rhs) const {
      return be32_t(rhs) == address;
    }
    bool operator!=(const uint32_t &rhs) const {
      return be32_t(rhs) != address;
    }

    bool FromString(std::string addr);
    std::string ToString() const;

    be32_t address;
  };

  enum Proto : uint8_t {
    kIcmp = 1,
    kTcp = 6,
    kUdp = 17,
    kRaw = 255,
  };

  std::string ToString() const;

  uint8_t version_ihl;
  uint8_t type_of_service;
  be16_t total_length;
  be16_t packet_id;
  be16_t fragment_offset;
  uint8_t time_to_live;
  uint8_t next_proto_id;
  uint16_t hdr_checksum;
  Address src_addr;
  Address dst_addr;
};

}  // namespace net
}  // namespace juggler

namespace std {
template <>
struct hash<juggler::net::Ipv4::Address> {
  std::size_t operator()(const juggler::net::Ipv4::Address &addr) const {
    return juggler::utils::hash<uint32_t>(
        reinterpret_cast<const char *>(&addr.address),
        sizeof(addr.address.raw_value()));
  }
};

}  // namespace std
#endif  // SRC_INCLUDE_IPV4_H_
