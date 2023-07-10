/**
 * @file l2fwd.cc
 * @brief A simple app that forwards packets back through the same port,
 * swapping source and destination MAC addresses.
 */

#include <dpdk.h>
#include <ether.h>
#include <glog/logging.h>
#include <packet.h>
#include <pmd.h>
#include <utils.h>

int main(int argc, const char *argv[]) {
  ::google::InitGoogleLogging(argv[0]);
  auto d = juggler::dpdk::Dpdk();
  const juggler::utils::CmdLineOpts kEalOpts(argc, argv);
  d.InitDpdk(kEalOpts);

  static constexpr uint8_t kPmdPort = 0;
  juggler::dpdk::PmdPort pmd_obj(
      kPmdPort, juggler::dpdk::PmdPort::kDefaultRingNr_,
      juggler::dpdk::PmdPort::kDefaultRingNr_,
      juggler::dpdk::PmdRing::kDefaultRingDescNr,
      juggler::dpdk::PmdRing::kDefaultRingDescNr * 2);

  pmd_obj.InitDriver();
  auto *rxring = pmd_obj.GetRing<juggler::dpdk::RxRing>(0);
  CHECK_NOTNULL(rxring);
  auto *txring = pmd_obj.GetRing<juggler::dpdk::TxRing>(0);
  CHECK_NOTNULL(txring);

  juggler::dpdk::PacketBatch batch;
  LOG(INFO) << "batch size: " << batch.GetRoom();
  while (true) {
    uint16_t net_rx = rxring->RecvPackets(&batch);

    for (uint16_t i = 0; i < net_rx; i++) {
      auto *in_pkt = batch.pkts()[i];
      auto *l2 =
          reinterpret_cast<juggler::net::Ethernet *>(in_pkt->head_data());
      auto tmp(l2->dst_addr);
      l2->dst_addr = l2->src_addr;
      l2->src_addr = tmp;
    }
    txring->SendPackets(&batch);
  }

  return 0;
}
