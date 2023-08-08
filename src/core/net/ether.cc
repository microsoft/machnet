#include <ether.h>

namespace juggler {
namespace net {

bool Ethernet::Address::FromString(std::string str) {
  return kSize == sscanf(str.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                         &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4],
                         &bytes[5]);
}

std::string Ethernet::Address::ToString() const {
  std::string ret;
  char addr[18];

  if (17 == snprintf(addr, sizeof(addr),
                     "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx", bytes[0],
                     bytes[1], bytes[2], bytes[3], bytes[4], bytes[5])) {
    ret = std::string(addr);
  }

  return ret;
}

std::string Ethernet::ToString() const {
  return juggler::utils::Format("[Eth: dst %s, src %s, eth_type %u]",
                                dst_addr.ToString().c_str(),
                                src_addr.ToString().c_str(), eth_type.value());
}

}  // namespace net
}  // namespace juggler
