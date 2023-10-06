/**
 * @file dpdk_test.cc
 *
 * Unit tests for juggler's DPDK helpers
 */
#include "dpdk.h"

#include <gtest/gtest.h>

#include <memory>

#include "packet.h"
#include "pmd.h"
#include "utils.h"

std::unique_ptr<juggler::dpdk::PacketPool> g_tx_pkt_pool;
std::unique_ptr<juggler::dpdk::PmdPort> g_pmd;

TEST(BasicTxTest, BasicTxTest) {
  const size_t payload_size = 4000;
  juggler::net::Ethernet::Address local_mac_addr("00:11:22:33:44:55");
  juggler::net::Ethernet::Address remote_mac_addr("00:11:22:33:44:55");
  auto local_ipv4_addr = juggler::net::Ipv4::Address::MakeAddress("1.1.1.1");
  CHECK(local_ipv4_addr.has_value());
  auto remote_ipv4_addr = juggler::net::Ipv4::Address::MakeAddress("2.2.2.2");
  CHECK(remote_ipv4_addr.has_value());

  juggler::dpdk::Packet *pkt = g_tx_pkt_pool->PacketAlloc();

  const size_t kTotalPacketLen = sizeof(juggler::net::Ethernet) +
                                 sizeof(juggler::net::Ipv4) + payload_size;
  auto *data = pkt->append(kTotalPacketLen);
  EXPECT_NE(data, nullptr);

  // L2 header
  auto *eh = pkt->head_data<juggler::net::Ethernet *>();
  eh->src_addr = local_mac_addr;
  eh->dst_addr = remote_mac_addr;
  eh->eth_type = juggler::be16_t(RTE_ETHER_TYPE_IPV4);

  // IPv4 header
  auto *ipv4h = pkt->head_data<juggler::net::Ipv4 *>(sizeof(*eh));
  ipv4h->version_ihl = 0x45;
  ipv4h->type_of_service = 0;
  ipv4h->packet_id = juggler::be16_t(0x1513);
  ipv4h->fragment_offset = juggler::be16_t(0);
  ipv4h->time_to_live = 64;
  ipv4h->next_proto_id = juggler::net::Ipv4::Proto::kUdp;
  ipv4h->total_length =
      juggler::be16_t(sizeof(juggler::net::Ipv4) +
                      sizeof(juggler::net::Ethernet) + payload_size);
  ipv4h->src_addr.address = juggler::be32_t(local_ipv4_addr.value().address);
  ipv4h->dst_addr.address = juggler::be32_t(remote_ipv4_addr.value().address);
  ipv4h->hdr_checksum = 0;

  EXPECT_EQ(pkt->length(), kTotalPacketLen);

  auto *txring = g_pmd->GetRing<juggler::dpdk::TxRing>(0);
  auto ret = txring->TrySendPackets(&pkt, 1);
  EXPECT_EQ(ret, 1);
}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);

  auto kEalOpts = juggler::utils::CmdLineOpts(
      {"-c", "0x0", "-n", "6", "--proc-type=auto", "-m", "1024", "--log-level",
       "8", "--vdev=net_null0,copy=1", "--no-pci"});

  auto d = juggler::dpdk::Dpdk();
  d.InitDpdk(kEalOpts);

  g_pmd.reset(new juggler::dpdk::PmdPort(
      0 /* because of PCIe allowlist, we have 1 port */));
  CHECK_NOTNULL(g_pmd);
  g_pmd->InitDriver();

  g_tx_pkt_pool.reset(new juggler::dpdk::PacketPool(
      juggler::dpdk::PmdRing::kDefaultRingDescNr * 2,
      juggler::dpdk::PmdRing::kJumboFrameSize + RTE_PKTMBUF_HEADROOM));
  CHECK_NOTNULL(g_tx_pkt_pool);

  return RUN_ALL_TESTS();
}
