/**
 * @file main.cc
 * @brief DPDK-based ping utility.
 */
#include <arp.h>
#include <dpdk.h>
#include <ether.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <icmp.h>
#include <ipv4.h>
#include <machnet_config.h>
#include <math.h>
#include <packet.h>
#include <pmd.h>
#include <ttime.h>
#include <utils.h>
#include <worker.h>

#include <csignal>
#include <iostream>
#include <optional>
#include <vector>

// The packets we send carry an ethernet, IPv4 and ICMP header.
// The payload includes at least a 64-bit unsigned integer. This is being used
// to attach the timestamp at packet creation (TSC).
constexpr uint16_t kMinPacketLength =
    std::max(sizeof(juggler::net::Ethernet) + sizeof(juggler::net::Ipv4) +
                 sizeof(juggler::net::Icmp) + 1 * sizeof(uint64_t),
             64ul);

DEFINE_string(
    config_json, "../src/apps/machnet/config.json",
    "JSON file with Juggler-related params for different hosts and binaries");
DEFINE_string(remote_ip, "", "Remote hostname in the hosts JSON file.");
DEFINE_uint32(icmp_id, 777, "ICMP ID to use in the packets.");
DEFINE_string(rtt_log, "", "Log file for RTT measurements.");

static volatile int g_keep_running = 1;

void int_handler([[maybe_unused]] int signal) { g_keep_running = 0; }

// Structure to keep application statistics related to TX and RX packets/errors.
struct stats {
  stats()
      : tx_success(0),
        tx_bytes(0),
        rx_count(0),
        rx_bytes(0),
        err_no_mbufs(0),
        err_tx_drops(0) {}
  uint64_t tx_success;
  uint64_t tx_bytes;
  uint64_t rx_count;
  uint64_t rx_bytes;
  uint64_t err_no_mbufs;
  uint64_t err_tx_drops;
};

/**
 * @brief This structure contains all the metadata required for all the routines
 * implemented in this application.
 */
struct task_context {
  /**
   * @param local_mac Local MAC address of the DPDK port in
   * `juggler::net::Ethernet::Address` format.
   * @param local_ip Local IP address to be used by the applicaton in
   * `juggler::net::Ipv4::Address` format.
   * @param remote_mac Remote host's MAC address in
   * `juggler::net::Ethernet::Address` format.
   * @param remote_ip Remote host's IP address to be used by the applicaton in
   * `juggler::net::Ipv4::Address` format.
   * @param packet_size Size of the packets to be generated.
   * @param rxring Pointer to the RX ring previously initialized.
   * @param txring Pointer to the TX ring previously initialized.
   */
  task_context(juggler::net::Ethernet::Address local_mac,
               juggler::net::Ipv4::Address local_ip,
               juggler::net::Ethernet::Address remote_mac,
               juggler::net::Ipv4::Address remote_ip,
               juggler::dpdk::RxRing *rxring, juggler::dpdk::TxRing *txring)
      : local_mac_addr(local_mac),
        local_ipv4_addr(local_ip),
        remote_mac_addr(remote_mac),
        remote_ipv4_addr(remote_ip),
        rx_ring(CHECK_NOTNULL(rxring)),
        tx_ring(CHECK_NOTNULL(txring)),
        arp_handler(local_mac, {local_ip}),
        statistics(),
        rtt_log() {}
  const juggler::net::Ethernet::Address local_mac_addr;
  const juggler::net::Ipv4::Address local_ipv4_addr;
  const juggler::net::Ethernet::Address remote_mac_addr;
  const juggler::net::Ipv4::Address remote_ipv4_addr;
  juggler::dpdk::RxRing *rx_ring;
  juggler::dpdk::TxRing *tx_ring;
  juggler::ArpHandler arp_handler;
  stats statistics;
  juggler::utils::TimeLog rtt_log;
};

void prepare_packet(void *context, juggler::dpdk::Packet *packet) {
  thread_local uint16_t icmp_seq = 0;
  auto *ctx = static_cast<task_context *>(context);

  CHECK_NOTNULL(packet->append(kMinPacketLength));

  // Prepare the L2 header.
  auto *eh = packet->head_data<juggler::net::Ethernet *>();
  eh->src_addr = ctx->local_mac_addr;
  eh->dst_addr = ctx->remote_mac_addr;
  eh->eth_type = juggler::be16_t(RTE_ETHER_TYPE_IPV4);
  packet->set_l2_len(sizeof(*eh));

  // Prepare the L3 header.
  auto *ipv4h = reinterpret_cast<juggler::net::Ipv4 *>(eh + 1);
  ipv4h->version_ihl = 0x45;
  ipv4h->type_of_service = 0;
  ipv4h->packet_id = juggler::be16_t(0x1513);
  ipv4h->fragment_offset = juggler::be16_t(0);
  ipv4h->time_to_live = juggler::net::Ipv4::kDefaultTTL;
  ipv4h->next_proto_id = juggler::net::Ipv4::Proto::kIcmp;
  ipv4h->total_length = juggler::be16_t(kMinPacketLength - sizeof(*eh));
  ipv4h->src_addr.address = juggler::be32_t(ctx->local_ipv4_addr.address);
  ipv4h->dst_addr.address = juggler::be32_t(ctx->remote_ipv4_addr.address);
  ipv4h->hdr_checksum = 0;
  packet->set_l3_len(sizeof(*ipv4h));

  // Prepare the L4 header.
  auto *icmph = reinterpret_cast<juggler::net::Icmp *>(ipv4h + 1);
  icmph->type = juggler::net::Icmp::Type::kEchoRequest;
  icmph->code = juggler::net::Icmp::kCodeZero;
  icmph->cksum = 0;
  icmph->id = juggler::be16_t(FLAGS_icmp_id);
  icmph->seq = juggler::be16_t(icmp_seq++);

  // Append the timestamp to the payload.
  auto *pkt_timestamp = reinterpret_cast<uint64_t *>(icmph + 1);
  *pkt_timestamp = juggler::time::rdtsc();

  // Compute the ICMP checksum.
  icmph->cksum = juggler::utils::ComputeChecksum16(
      reinterpret_cast<uint8_t *>(icmph),
      kMinPacketLength - sizeof(*eh) - sizeof(*ipv4h));

  // Offload IPv4 and UDP checksums to hardware.
  packet->offload_ipv4_csum();
}

/**
 * @brief This is a ping routine that sends a packet to the remote host and
 * measures the RTT.
 * @param now Current timestamp in nanoseconds.
 * @param context Opaque pointer to the task context.
 */
void ping(uint64_t now, void *context) {
  static thread_local uint64_t last_ping_time;
  auto *ctx = static_cast<task_context *>(context);
  auto *tx = ctx->tx_ring;
  auto *pp = tx->GetPacketPool();
  auto *st = &ctx->statistics;

  if (now - last_ping_time >= juggler::time::ms_to_cycles(1000)) {
    auto *packet = pp->PacketAlloc();
    if (packet == nullptr) {
      st->err_no_mbufs++;
      return;
    }

    prepare_packet(context, packet);
    tx->SendPackets(&packet, 1);
    st->tx_success++;
    st->tx_bytes += kMinPacketLength;
    last_ping_time = now;
  }
}

/**
 * @brief Helper function to report TX/RX statistics at second granularity. It
 * keeps a checkpoint of the statistics since the previous report to calculate
 * per-second statistics.
 *
 * @param now Current TSC.
 * @param cur Pointer to the main statistics object.
 */
void report_stats(uint64_t now, void *context) {
  static const size_t kGiga = 1E9;
  thread_local uint64_t last_report_timestamp;
  thread_local stats stats_checkpoint;
  const auto *ctx = static_cast<task_context *>(context);
  const stats *cur = &ctx->statistics;

  auto cycles_elapsed = now - last_report_timestamp;
  if (cycles_elapsed > juggler::time::s_to_cycles(1)) {
    auto sec_elapsed = juggler::time::cycles_to_s<double>(cycles_elapsed);
    auto packets_sent = cur->tx_success - stats_checkpoint.tx_success;
    auto tx_pps = static_cast<double>(packets_sent) / sec_elapsed;
    auto tx_gbps =
        static_cast<double>(cur->tx_bytes - stats_checkpoint.tx_bytes) /
        sec_elapsed * 8.0 / kGiga;
    auto packets_received = cur->rx_count - stats_checkpoint.rx_count;
    auto rx_pps = static_cast<double>(packets_received / sec_elapsed);
    auto rx_gbps =
        static_cast<double>(cur->rx_bytes - stats_checkpoint.rx_bytes) /
        sec_elapsed * 8.0 / kGiga;
    auto packets_dropped = cur->err_tx_drops - stats_checkpoint.err_tx_drops;
    auto tx_drop_pps = static_cast<double>(packets_dropped / sec_elapsed);

    LOG(INFO) << juggler::utils::Format(
        "[TX PPS: %lf (%lf Gbps), RX PPS: %lf (%lf Gbps), TX_DROP PPS: %lf]",
        tx_pps, tx_gbps, rx_pps, rx_gbps, tx_drop_pps);

    // Update local variables for next report.
    stats_checkpoint = *cur;
    last_report_timestamp = now;
  }
}

void report_final_stats(void *context) {
  auto *ctx = static_cast<task_context *>(context);
  const stats *st = &ctx->statistics;
  auto *rtt_log = &ctx->rtt_log;

  using stats_tuple =
      std::tuple<double, double, size_t, size_t, size_t, size_t, size_t>;
  decltype(auto) rtt_stats_calc =
      [](std::vector<uint64_t> &samples) -> std::optional<stats_tuple> {
    uint64_t sum = 0;
    uint64_t count = 0;
    for (const auto &sample : samples) {
      if (sample == 0) break;
      sum += sample;
      count++;
    }
    if (count == 0) {
      return std::nullopt;
    }
    double mean = static_cast<double>(sum) / count;
    double variance = 0;
    for (const auto &sample : samples) {
      if (sample == 0) break;
      variance += (sample - mean) * (sample - mean);
    }
    variance /= count;
    double stddev = std::sqrt(variance);

    sort(samples.begin(), samples.end());

    auto min = samples[0];
    auto p50 = samples[static_cast<size_t>(count * 0.5)];
    auto p99 = samples[static_cast<size_t>(count * 0.99)];
    auto p999 = samples[static_cast<size_t>(count * 0.999)];
    auto max = samples[count - 1];

    return std::make_tuple(mean, stddev, p50, p99, p999, min, max);
  };

  if (FLAGS_rtt_log != "") {
    rtt_log->DumpToFile(FLAGS_rtt_log);
  }

  auto rtt_stats = rtt_log->Apply<std::optional<stats_tuple>>(rtt_stats_calc);
  if (!rtt_stats.has_value()) {
    LOG(INFO) << "RTT (ns): no samples";
    return;
  }
  auto [mean, stddev, p50, p99, p999, min, max] = rtt_stats.value();

  std::cout << juggler::utils::Format("--- RTT %s ping statistics ---\n",
                                      ctx->remote_ipv4_addr.ToString().c_str());
  std::cout << juggler::utils::Format(
      "%lu packets transmitted, %lu received, "
      "%.2lf%% packet loss, mean %.2lf us, p50 %.2lf us, p99 %.2lf us, p999 "
      "%.2lf us, min %.2lf us, max %.2lf us\n",
      st->tx_success, st->rx_count,
      100.0 * (st->tx_success - st->rx_count) / st->tx_success, mean / 1000.0,
      p50 / 1000.0, p99 / 1000.0, p999 / 1000.0, min / 1000.0, max / 1000.0);
  LOG(INFO) << juggler::utils::Format(
      "Application Statistics (TOTAL) - [TX] Sent: %lu, Drops: %lu, "
      "DropsNoMbuf: %lu "
      "[RX] Received: %lu",
      st->tx_success, st->err_tx_drops, st->err_no_mbufs, st->rx_count);
}

// Main network receive routine.
// ICMP echo reply packets are handled here.
void icmp_rx(void *context) {
  thread_local uint16_t expected_icmp_seq;
  auto ctx = static_cast<task_context *>(context);
  const auto &local_mac = ctx->local_mac_addr;
  auto *rx = ctx->rx_ring;
  auto *st = &ctx->statistics;
  auto *rtt_log = &ctx->rtt_log;

  juggler::dpdk::PacketBatch batch;
  auto packets_received = rx->RecvPackets(&batch);
  for (uint16_t i = 0; i < packets_received; i++) {
    auto *packet = batch[i];

    // Get the Ethernet header.
    auto *eh = packet->head_data<juggler::net::Ethernet *>();
    if (eh->eth_type.value() == juggler::net::Ethernet::EthType::kArp) {
      // Process ARP packets.
      const auto *arph = packet->head_data<juggler::net::Arp *>(
          sizeof(juggler::net::Ethernet));
      ctx->arp_handler.ProcessArpPacket(ctx->tx_ring, arph);
      continue;
    }

    if (eh->dst_addr != local_mac) {
      // This packet is not for us.
      continue;
    }

    if (eh->eth_type.value() != juggler::net::Ethernet::kIpv4) {
      continue;
    }

    // Get the IPv4 header.
    auto *ipv4h =
        packet->head_data<juggler::net::Ipv4 *>(sizeof(juggler::net::Ethernet));
    if (ipv4h->dst_addr != ctx->local_ipv4_addr) {
      // This packet is not for us.
      continue;
    }

    if (ipv4h->next_proto_id != juggler::net::Ipv4::kIcmp) {
      continue;
    }

    // Get the ICMP header.
    auto *icmph = packet->head_data<juggler::net::Icmp *>(
        sizeof(juggler::net::Ethernet) + sizeof(juggler::net::Ipv4));
    if (icmph->type != juggler::net::Icmp::kEchoReply) {
      continue;
    }
    // Check the ICMP id.
    if (icmph->id.value() != FLAGS_icmp_id) {
      continue;
    }

    // Check the ICMP sequence number.
    if (icmph->seq.value() != expected_icmp_seq) {
      LOG(WARNING) << "ICMP sequence number mismatch. Expected: "
                   << expected_icmp_seq << ", got: " << icmph->seq.value();
      // Update the expected ICMP sequence number.
      if (icmph->seq.value() > expected_icmp_seq) {
        expected_icmp_seq = icmph->seq.value();
      }
      continue;
    } else {
      expected_icmp_seq++;
    }

    st->rx_bytes += packet->length();
    st->rx_count++;
    auto *tx_timestamp = reinterpret_cast<uint64_t *>(icmph + 1);
    auto rtt_ns =
        juggler::time::cycles_to_ns(juggler::time::rdtsc() - *tx_timestamp);
    rtt_log->Record(rtt_ns);
    std::cout << packet->length() << " bytes from "
              << ipv4h->src_addr.ToString()
              << ": icmp_seq=" << icmph->seq.value()
              << " ttl=" << static_cast<int>(ipv4h->time_to_live)
              << " time=" << static_cast<double>(rtt_ns) / 1000.0 << " us"
              << std::endl;
  }
  // We need to release the received packet mbufs back to the pool.
  batch.Release();
}

std::optional<juggler::net::Ethernet::Address> ArpResolveBusyWait(
    const juggler::net::Ethernet::Address &local_mac,
    const juggler::net::Ipv4::Address &local_ip, juggler::dpdk::TxRing *tx_ring,
    juggler::dpdk::RxRing *rx_ring,
    const juggler::net::Ipv4::Address &remote_ip, int timeout_in_sec) {
  juggler::ArpHandler arp_handler(local_mac, {local_ip});

  arp_handler.GetL2Addr(tx_ring, local_ip, remote_ip);
  auto start = std::chrono::steady_clock::now();
  while (true) {
    auto now = std::chrono::steady_clock::now();
    if (now - start > std::chrono::seconds(timeout_in_sec)) {
      LOG(ERROR) << "ARP resolution timed out.";
      return std::nullopt;
    }

    juggler::dpdk::PacketBatch batch;
    auto nr_rx = rx_ring->RecvPackets(&batch);
    for (uint16_t i = 0; i < nr_rx; i++) {
      const auto *packet = batch[i];
      if (packet->length() <
          sizeof(juggler::net::Ethernet) + sizeof(juggler::net::Arp)) {
        continue;
      }
      auto *eh = packet->head_data<juggler::net::Ethernet *>();
      if (eh->eth_type.value() != juggler::net::Ethernet::kArp) {
        continue;
      }

      auto *arph = packet->head_data<juggler::net::Arp *>(
          sizeof(juggler::net::Ethernet));
      arp_handler.ProcessArpPacket(tx_ring, arph);
      auto remote_mac = arp_handler.GetL2Addr(tx_ring, local_ip, remote_ip);
      if (remote_mac.has_value()) {
        batch.Release();
        return remote_mac;
      }
    }
    batch.Release();
  }
}

int main(int argc, char *argv[]) {
  ::google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  gflags::SetUsageMessage("Simple packet generator.");

  signal(SIGINT, int_handler);
  juggler::MachnetConfigProcessor config_processor(FLAGS_config_json);

  LOG_IF(FATAL, config_processor.interfaces_config().size() != 1)
      << "Exactly one interface must be configured. ";

  auto dpdk = juggler::dpdk::Dpdk();
  dpdk.InitDpdk(config_processor.GetEalOpts());
  if (dpdk.GetNumPmdPortsAvailable() == 0) {
    LOG(ERROR) << "No PMD ports available.";
    return -1;
  }

  const auto &interface_config = config_processor.interfaces_config().begin();
  auto pmd_port_id = dpdk.GetPmdPortIdByMac(interface_config->l2_addr());
  if (!pmd_port_id) {
    LOG(ERROR) << "No PMD port found for MAC address: "
               << interface_config->l2_addr().ToString();
    return -1;
  }

  const auto nic_queues_nr = 1;
  juggler::dpdk::PmdPort pmd_port(pmd_port_id.value(), nic_queues_nr,
                                  nic_queues_nr,
                                  juggler::dpdk::PmdRing::kDefaultRingDescNr,
                                  juggler::dpdk::PmdRing::kDefaultRingDescNr);
  pmd_port.InitDriver();

  auto *rxring = pmd_port.GetRing<juggler::dpdk::RxRing>(0);
  CHECK_NOTNULL(rxring);
  auto *txring = pmd_port.GetRing<juggler::dpdk::TxRing>(0);
  CHECK_NOTNULL(txring);

  if (FLAGS_remote_ip == "") {
    std::cerr << "Remote IP address not specified.";
    return -1;
  }

  juggler::net::Ipv4::Address remote_ip;
  CHECK(remote_ip.FromString(FLAGS_remote_ip)) << "Invalid remote IP address.";

  const auto remote_mac = ArpResolveBusyWait(interface_config->l2_addr(),
                                             interface_config->ip_addr(),
                                             txring, rxring, remote_ip, 5);
  if (!remote_mac.has_value()) {
    LOG(ERROR) << "ARP resolution failed.";
    return -1;
  }

  task_context task_ctx(interface_config->l2_addr(),
                        interface_config->ip_addr(), remote_mac.value(),
                        remote_ip, rxring, txring);

  auto pingpong = [](uint64_t now, void *context) {
    ping(now, context);
    icmp_rx(context);
  };

  auto pingpong_task =
      std::make_shared<juggler::Task>(pingpong, static_cast<void *>(&task_ctx));

  std::vector<std::shared_ptr<juggler::Task>> tasks = {pingpong_task};
  std::vector<cpu_set_t> cpu_masks = {interface_config->cpu_mask()};
  juggler::WorkerPool<juggler::Task> WPool(tasks, cpu_masks);

  WPool.Init();

  std::cout << "PING " << FLAGS_remote_ip << " (" << remote_ip.ToString()
            << ") 56(84) bytes of data." << std::endl;
  // Launch the worker pool.
  WPool.Launch();

  while (g_keep_running) {
    sleep(5);
  }

  WPool.Pause();
  WPool.Terminate();
  report_final_stats(&task_ctx);
  pmd_port.DumpStats();

  return (0);
}
