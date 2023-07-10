#include <udp.h>

namespace juggler {
namespace net {

std::string Udp::ToString() const {
  return juggler::utils::Format(
      "[UDP: src_port %zu, dst_port %zu, len %zu, csum %zu]",
      src_port.port.value(), dst_port.port.value(), len.value(), cksum.value());
}

}  // namespace net
}  // namespace juggler
