#include <ipv4.h>

#include <optional>

namespace juggler {
namespace net {

bool Ipv4::Address::IsValid(const std::string &addr) {
  struct sockaddr_in sa;
  int result = inet_pton(AF_INET, addr.c_str(), &(sa.sin_addr));
  return result != 0;
}

std::optional<Ipv4::Address> Ipv4::Address::MakeAddress(
    const std::string &addr) {
  Address ret;
  if (!ret.FromString(addr)) return std::nullopt;
  return ret;
}

bool Ipv4::Address::FromString(std::string str) {
  if (!Ipv4::Address::IsValid(str)) return false;
  unsigned char bytes[4];
  uint8_t len = sscanf(str.c_str(), "%hhu.%hhu.%hhu.%hhu", &bytes[0], &bytes[1],
                       &bytes[2], &bytes[3]);
  if (len != Ipv4::Address::kSize) return false;
  address = be32_t((uint32_t)(bytes[0]) << 24 | (uint32_t)(bytes[1]) << 16 |
                   (uint32_t)(bytes[2]) << 8 | (uint32_t)(bytes[3]));

  return true;
}

std::string Ipv4::Address::ToString() const {
  const std::vector<uint8_t> bytes(address.ToByteVector());
  CHECK_EQ(bytes.size(), 4);
  return juggler::utils::Format("%hhu.%hhu.%hhu.%hhu", bytes[0], bytes[1],
                                bytes[2], bytes[3]);
}

std::string Ipv4::ToString() const {
  return juggler::utils::Format(
      "[IPv4: src %s, dst %s, ihl %u, ToS %u, tot_len %u, ID %u, frag_off %u, "
      "TTL %u, proto %u, check %u]",
      src_addr.ToString().c_str(), dst_addr.ToString().c_str(), version_ihl,
      type_of_service, total_length.value(), packet_id.value(),
      fragment_offset.value(), time_to_live, next_proto_id, hdr_checksum);
}

}  // namespace net
}  // namespace juggler
