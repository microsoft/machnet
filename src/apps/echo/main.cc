/**
 * @file main.cc
 * @brief Simple echo server implementation using core library.
 */
#include <dpdk.h>
#include <ether.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <packet.h>
#include <pmd.h>
#include <utils.h>
#include <worker.h>

#include <csignal>
#include <iostream>
#include <vector>

#include "config_json_reader.h"

DEFINE_string(
    config_json, "../../../servers.json",
    "JSON file with Juggler-related params for different hosts and binaries");
DEFINE_string(local_hostname, "", "Local hostname in the hosts JSON file.");

static volatile int g_keep_running = 1;

void int_handler([[maybe_unused]] int signal) { g_keep_running = 0; }

struct task_context {
  task_context(juggler::dpdk::PacketPool *pp, juggler::dpdk::RxRing *rxring,
               juggler::dpdk::TxRing *txring)
      : packet_pool(pp), rx_ring(rxring), tx_ring(txring) {}

  juggler::dpdk::PacketPool *packet_pool;
  juggler::dpdk::RxRing *rx_ring;
  juggler::dpdk::TxRing *tx_ring;
};

void task_main(uint64_t now, void *context) {
  auto ctx = static_cast<task_context *>(context);
  auto rx = ctx->rx_ring;
  auto tx = ctx->tx_ring;
  auto pp = ctx->packet_pool;

  juggler::dpdk::PacketBatch batch;
  uint16_t net_rx = rx->RecvPackets(&batch);
  for (auto i = 0; i < net_rx; i++) {
    auto *in_pkt = batch.pkts()[i];
    auto *out_pkt = pp->PacketAlloc();
    CHECK_NOTNULL(out_pkt);
    LOG(INFO) << "packet length: " << in_pkt->length();
    void *dst = out_pkt->append(in_pkt->length());
    CHECK_NOTNULL(dst);

    void *src = in_pkt->head_data();
    juggler::utils::Copy(dst, src, in_pkt->length());

    auto *eh = reinterpret_cast<juggler::net::Ethernet *>(dst);
    LOG(INFO) << "Src L2addr: " << eh->src_addr.ToString();
    LOG(INFO) << "Dst L2addr: " << eh->dst_addr.ToString();
    juggler::net::Ethernet::Address tmp;
    juggler::utils::Copy(&tmp, &eh->dst_addr, sizeof(tmp));
    juggler::utils::Copy(&eh->dst_addr, &eh->src_addr, sizeof(eh->dst_addr));
    juggler::utils::Copy(&eh->src_addr, &tmp, sizeof(eh->src_addr));

    tx->SendPackets(&out_pkt, 1);
  }

  // Clear the batch; this reclaims mbufs.
  batch.Clear();
}

int main(int argc, char *argv[]) {
  ::google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  gflags::SetUsageMessage("Simple packet bouncing server.");

  signal(SIGINT, int_handler);
  JugglerConfigJsonReader config_json(FLAGS_config_json);

  auto d = juggler::dpdk::Dpdk();

  // Construct `CmdLineOpts` from user-provided EAL arguments, if any.
  auto eal_opts = juggler::utils::CmdLineOpts(
      config_json.GetEalArgsForHost(FLAGS_local_hostname));

  if (eal_opts.IsEmpty()) {
    LOG(WARNING) << "Using default EAL options.";
    eal_opts = juggler::dpdk::kDefaultEalOpts;
  }

  // A PCIe-allow address is required.
  eal_opts.Append(
      {"-a", config_json.GetPcieAllowlistForHost(FLAGS_local_hostname)});

  d.InitDpdk(eal_opts);
  auto packet_pool = std::make_unique<juggler::dpdk::PacketPool>(2048);
  juggler::dpdk::PmdPort pmd_obj(
      config_json.GetPmdPortIdForHost(FLAGS_local_hostname));
  pmd_obj.InitDriver();

  auto *rxring = pmd_obj.GetRing<juggler::dpdk::RxRing>(0);
  CHECK_NOTNULL(rxring);
  auto *txring = pmd_obj.GetRing<juggler::dpdk::TxRing>(0);
  CHECK_NOTNULL(txring);

  task_context task_ctx(packet_pool.get(), rxring, txring);
  std::vector<std::shared_ptr<juggler::Task>> tasks;
  tasks.emplace_back(std::make_shared<juggler::Task>(
      &task_main, static_cast<void *>(&task_ctx)));
  std::vector<uint8_t> cpu_cores;
  juggler::WorkerPool<juggler::Task> WPool(tasks, {cpu_cores});
  WPool.Init();

  // Set worker to running.
  WPool.Launch();

  while (g_keep_running) {
    sleep(5);
  }

  WPool.Pause();
  WPool.Terminate();

  return (0);
}
