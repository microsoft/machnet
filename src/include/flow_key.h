/**
 * @file flow_key.h
 */
#ifndef SRC_INCLUDE_FLOW_KEY_H_
#define SRC_INCLUDE_FLOW_KEY_H_

#include <ipv4.h>
#include <udp.h>

namespace juggler {
namespace net {
namespace flow {

struct Listener {
  using Ipv4 = juggler::net::Ipv4;
  using Udp = juggler::net::Udp;
  Listener(const Listener& other) = default;

  /**
   * @brief Construct a new Listener object.
   *
   * @param local_addr Local IP address (in network byte order).
   * @param local_port Local UDP port (in network byte order).
   */
  Listener(const Ipv4::Address& local_addr, const Udp::Port& local_port)
      : addr(local_addr), port(local_port) {}

  /**
   * @brief Construct a new Listener object.
   *
   * @param local_addr Local IP address (in host byte order).
   * @param local_port Local UDP port (in host byte order).
   */
  Listener(const uint32_t local_addr, const uint16_t local_port)
      : addr(local_addr), port(local_port) {}

  bool operator==(const Listener& other) const {
    return addr == other.addr && port == other.port;
  }

  const Ipv4::Address addr;
  const Udp::Port port;
};
static_assert(sizeof(Listener) == 6, "Listener size is not 6 bytes.");

/**
 * @struct Key
 * @brief Flow key: corresponds to the 5-tuple (UDP is always the protocol).
 */
struct Key {
  using Ipv4 = juggler::net::Ipv4;
  using Udp = juggler::net::Udp;
  Key(const Key& other) = default;
  /**
   * @brief Construct a new Key object.
   *
   * @param local_addr Local IP address (in network byte order).
   * @param local_port Local UDP port (in network byte order).
   * @param remote_addr Remote IP address (in network byte order).
   * @param remote_port Remote UDP port (in network byte order).
   */
  Key(const Ipv4::Address& local_addr, const Udp::Port& local_port,
      const Ipv4::Address& remote_addr, const Udp::Port& remote_port)
      : local_addr(local_addr),
        local_port(local_port),
        remote_addr(remote_addr),
        remote_port(remote_port) {}

  /**
   * @brief Construct a new Key object.
   *
   * @param local_addr Local IP address (in host byte order).
   * @param local_port Local UDP port (in host byte order).
   * @param remote_addr Remote IP address (in host byte order).
   * @param remote_port Remote UDP port (in host byte order).
   */
  Key(const uint32_t local_addr, const uint16_t local_port,
      const uint32_t remote_addr, const uint16_t remote_port)
      : local_addr(local_addr),
        local_port(local_port),
        remote_addr(remote_addr),
        remote_port(remote_port) {}

  bool operator==(const Key& other) const {
    return local_addr == other.local_addr && local_port == other.local_port &&
           remote_addr == other.remote_addr && remote_port == other.remote_port;
  }

  std::string ToString() const {
    return utils::Format("[%s:%hu <-> %s:%hu]", remote_addr.ToString().c_str(),
                         remote_port.port.value(),
                         local_addr.ToString().c_str(),
                         local_port.port.value());
  }

  const Ipv4::Address local_addr;
  const Udp::Port local_port;
  const Ipv4::Address remote_addr;
  const Udp::Port remote_port;
};
static_assert(sizeof(Key) == 12, "Flow key size is not 12 bytes.");

}  // namespace flow
}  // namespace net
}  // namespace juggler

namespace std {

template <>
struct hash<juggler::net::flow::Listener> {
  size_t operator()(const juggler::net::flow::Listener& listener) const {
    return juggler::utils::hash<uint64_t>(
        reinterpret_cast<const char*>(&listener), sizeof(listener));
  }
};

template <>
struct hash<juggler::net::flow::Key> {
  size_t operator()(const juggler::net::flow::Key& key) const {
    // TODO(ilias): Use a better hash function.
    // return std::hash<uint32_t>()(key.local_addr.address.raw_value()) ^
    //        std::hash<uint32_t>()(key.remote_addr.address.raw_value()) ^
    //        std::hash<uint16_t>()(key.local_port.port.raw_value()) ^
    //        std::hash<uint16_t>()(key.remote_port.port.raw_value());
    return juggler::utils::hash<uint64_t>(reinterpret_cast<const char*>(&key),
                                          sizeof(key));
  }
};

}  // namespace std

#endif  // SRC_INCLUDE_FLOW_KEY_H_
