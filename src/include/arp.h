/**
 * @file arp.h
 * @brief This file contains the ARP header definitions, and an ARP handler
 * class.
 */
#ifndef SRC_INCLUDE_ARP_H_
#define SRC_INCLUDE_ARP_H_

#include <common.h>
#include <dpdk.h>
#include <ether.h>
#include <ipv4.h>
#include <packet.h>
#include <pmd.h>

#include <unordered_set>

namespace juggler {
namespace net {

// ARP packet header for IPv4 addresses.
// NOTE: IPv6 is not supported yet.
struct __attribute__((packed, aligned(2))) Arp {
  struct __attribute__((packed, aligned(2))) Ipv4Data {
    Ipv4Data() = default;
    Ipv4Data(Ethernet::Address src_mac, Ipv4::Address src_ip,
             Ethernet::Address target_mac, Ipv4::Address target_ip)
        : sha(src_mac), spa(src_ip), tha(target_mac), tpa(target_ip) {}
    Ethernet::Address sha;  // Sender Hardware Address
    Ipv4::Address spa;      // Sender Protocol Address
    Ethernet::Address tha;  // Target Hardware Address
    Ipv4::Address tpa;      // Target Protocol Address
  };
  static_assert(sizeof(Ipv4Data) == 20, "Arp::Ipv4Data size mismatch");

  enum ArpHwType : uint8_t {
    kEthernet = 1,
  };

  enum ArpHlen : uint8_t {
    kEthernetLen = 6,
  };

  enum ArpPlen : uint8_t {
    kIpv4Len = 4,
  };

  enum ArpOp : uint16_t {
    kRequest = 1,
    kReply = 2,
    kRevRequest = 3,
    kRevReply = 4,
    kInvRequest = 8,
    kInvReply = 9,
  };

  be16_t htype;  // Hardware Type
  be16_t ptype;  // Protocol Type (ptype space is shared with EthType)
  uint8_t hlen;  // Hardware Address Length
  uint8_t plen;  // Protocol Address Length
  be16_t op;     // Operation
  Ipv4Data ipv4_data;
};
static_assert(sizeof(Arp) == 28, "Arp size mismatch");

}  // namespace net

/**
 * @brief This class implements a minimal ARP layer. It is used to resolve IP
 * addresses to MAC addresses.
 *
 * This class is not thread-safe.
 *
 * NOTE: No IPv6 support yet.
 */
class ArpHandler {
 public:
  using Ethernet = net::Ethernet;
  using Arp = net::Arp;
  using Ipv4 = net::Ipv4;
  ArpHandler() = delete;
  ArpHandler(Ethernet::Address l2addr,
             std::vector<Ipv4::Address> local_ip_addrs)
      : local_l2addr_(l2addr) {
    for (const auto &ip_addr : local_ip_addrs) {
      CHECK(local_ip_addrs_.find(ip_addr) == local_ip_addrs_.end());
      local_ip_addrs_.insert(ip_addr);
    }
  }

  /**
   * @brief This method is used to add a new IP address to the pool of local IP
  addresses in this handler.
   * An ARP request sent for this IP address, will be answered by this handler.
   *
   * @param ip_addr The IP address to add.
   */
  void AddLocalIpAddr(const Ipv4::Address &ip_addr) {
    if (local_ip_addrs_.find(ip_addr) != local_ip_addrs_.end()) return;

    local_ip_addrs_.insert(ip_addr);
  }

  /**
   * @brief This method is called to issue an ARP who-has request in the LAN.
   * The system owning the remote IP needs to respond with an ARP reply.
   *
   * @param local_ip  The IP address of the local machine.
   * @param target_ip The IP address of the target machine.
   */
  void RequestL2Addr(const dpdk::TxRing *txring, const Ipv4::Address &local_ip,
                     const Ipv4::Address &target_ip) {
    DCHECK_NOTNULL(txring);
    DCHECK_NOTNULL(txring->GetPacketPool());
    auto *packet = txring->GetPacketPool()->PacketAlloc();
    CHECK_NOTNULL(packet);

    auto *eh = packet->append<Ethernet *>(sizeof(Ethernet) + sizeof(Arp));
    CHECK_NOTNULL(eh);

    // L2 header.
    eh->src_addr = local_l2addr_;
    eh->dst_addr = net::Ethernet::kBroadcastAddr;
    eh->eth_type = be16_t(net::Ethernet::kArp);

    // ARP header.
    auto *arph = reinterpret_cast<Arp *>(eh + 1);
    arph->htype = be16_t(net::Arp::ArpHwType::kEthernet);
    arph->ptype = be16_t(net::Ethernet::EthType::kIpv4);
    arph->hlen = Arp::ArpHlen::kEthernetLen;
    arph->plen = Arp::ArpPlen::kIpv4Len;
    arph->op = be16_t(net::Arp::ArpOp::kRequest);
    DCHECK(local_ip_addrs_.find(local_ip) != local_ip_addrs_.end())
        << "Local IP not registered.";
    arph->ipv4_data.sha = local_l2addr_;
    arph->ipv4_data.spa = local_ip;
    arph->ipv4_data.tha = net::Ethernet::kZeroAddr;
    arph->ipv4_data.tpa = target_ip;

    // Send the ARP request.
    auto nb_tx = txring->TrySendPackets(&packet, 1);
    LOG_IF(WARNING, nb_tx != 1) << "Failed to send ARP request";
  }

  /**
   * @brief Generate an ARP reply packet.
   *
   * @param rx_arph A pointer to the received packet's ARP header.
   * @param local_ip local IP address which will be used to generate the reply.
   */
  void Reply(const dpdk::TxRing *txring, const Arp *rx_arph,
             const Ipv4::Address &local_ip) const {
    DCHECK_NOTNULL(txring);
    DCHECK_NOTNULL(txring->GetPacketPool());
    auto *packet = txring->GetPacketPool()->PacketAlloc();
    CHECK_NOTNULL(packet);

    auto *eh = packet->append<Ethernet *>(sizeof(Ethernet) + sizeof(Arp));
    CHECK_NOTNULL(eh);

    // L2 header.
    eh->src_addr = local_l2addr_;
    eh->dst_addr = rx_arph->ipv4_data.sha;
    eh->eth_type = be16_t(Ethernet::kArp);

    // ARP header.
    auto *arph = reinterpret_cast<Arp *>(eh + 1);
    arph->htype = be16_t(Arp::ArpHwType::kEthernet);
    arph->ptype = be16_t(Ethernet::EthType::kIpv4);
    arph->hlen = Arp::ArpHlen::kEthernetLen;
    arph->plen = Arp::ArpPlen::kIpv4Len;
    arph->op = be16_t(Arp::ArpOp::kReply);
    arph->ipv4_data.sha = local_l2addr_;
    arph->ipv4_data.spa = local_ip;
    arph->ipv4_data.tha = rx_arph->ipv4_data.sha;
    arph->ipv4_data.tpa = rx_arph->ipv4_data.spa;

    // Send the ARP reply.
    auto nb_tx = txring->TrySendPackets(&packet, 1);
    LOG_IF(WARNING, nb_tx != 1) << "Failed to send ARP reply";
  }

  /**
   * @brief This method is being called to resolve a target IP's MAC address.
   *
   * @param txring The Tx ring to use for sending ARP requests (packet pool
   *               needs to attached).
   * @param local_ip  The IP address of the local machine.
   * @param target_ip The IP address of the target machine.
   * @return The MAC address of the target machine, if found in the cache, or
   *        `std::nullopt` otherwise.
   */
  std::optional<Ethernet::Address> GetL2Addr(const dpdk::TxRing *txring,
                                             const Ipv4::Address &local_ip,
                                             const Ipv4::Address &target_ip) {
    if (arp_table_.find(target_ip) != arp_table_.end()) {
      return arp_table_[target_ip];
    }

    // Destination L2 Adress not found in the cache; issue an ARP who-has
    // request for the given target IP address.
    RequestL2Addr(txring, local_ip, target_ip);
    return std::nullopt;
  }

  /**
   * @brief This method is called when an ARP packet is received.
   *
   * @param arph The ARP header.
   */
  void ProcessArpPacket(dpdk::TxRing *txring, const Arp *arph) {
    DCHECK(arph != nullptr);

    // We do not need to do any L2 processing; already took place.
    // Sanity checks.
    if (arph->htype.value() != Arp::ArpHwType::kEthernet)  // NOLINT
        [[unlikely]] {                                     // NOLINT
      LOG(WARNING) << "Received ARP packet with invalid hardware type: "
                   << arph->htype.value();
      return;
    }

    if (arph->ptype.value() != Ethernet::EthType::kIpv4)  // NOLINT
        [[unlikely]] {                                    // NOLINT
      LOG(WARNING) << "Received a non-ipv4 ARP packet.";
      return;
    }

    if (arph->hlen != Arp::ArpHlen::kEthernetLen ||  // NOLINT
        arph->plen != Arp::ArpPlen::kIpv4Len)        // NOLINT
        [[unlikely]] {                               // NOLINT
      LOG(WARNING) << "Received ARP packet with invalid hardware or protocol "
                   << "address length.";
      return;
    }

    static const Ipv4::Address zero_addr(0u);
    auto target_ip = arph->ipv4_data.tpa;
    // Check ARP opcode, and handle operation.
    switch (arph->op.value()) {
      case Arp::ArpOp::kRequest:
        // Check if this request is for us.
        if (local_ip_addrs_.find(target_ip) == local_ip_addrs_.end()) break;
        Reply(txring, arph, target_ip);
        break;
      case Arp::ArpOp::kReply:
        // Check if the ARP reply is for us.
        if (local_ip_addrs_.find(target_ip) == local_ip_addrs_.end()) break;
        // Update the cache.
        arp_table_[arph->ipv4_data.spa] = arph->ipv4_data.sha;
        break;
      default:
        LOG(WARNING) << "Received ARP packet with unsupported operation.";
        return;
    }
  }

  std::vector<std::tuple<std::string, std::string>> GetArpTableEntries() const {
    std::vector<std::tuple<std::string, std::string>> arp_table;
    for (auto &kv : arp_table_) {
      std::string ip_addr = kv.first.ToString();
      std::string l2_addr = kv.second.ToString();
      arp_table.emplace_back(ip_addr, l2_addr);
    }
    return arp_table;
  }

  size_t GetArpTableSize() const { return arp_table_.size(); }

 private:
  const Ethernet::Address local_l2addr_;
  std::unordered_set<Ipv4::Address> local_ip_addrs_;
  std::unordered_map<Ipv4::Address, Ethernet::Address> arp_table_{};
};

}  // namespace juggler

#endif  // SRC_INCLUDE_ARP_H_
