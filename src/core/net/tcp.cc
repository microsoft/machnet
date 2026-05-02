#include <tcp.h>

namespace juggler {
namespace net {

std::string Tcp::ToString() const {
  return juggler::utils::Format(
      "[TCP: src_port %u, dst_port %u, seq %u, ack %u, flags 0x%02x, "
      "win %u]",
      static_cast<unsigned>(src_port.port.value()),
      static_cast<unsigned>(dst_port.port.value()), seq_num.value(),
      ack_num.value(), static_cast<unsigned>(flags),
      static_cast<unsigned>(window.value()));
}

}  // namespace net
}  // namespace juggler
