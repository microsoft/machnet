/**
 * @file main.cc
 * @brief Simple pkt generator application using core library.
 */
#include <arp.h>
#include <dpdk.h>
#include <ether.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <ipv4.h>
#include <machnet_config.h>
#include <math.h>
#include <packet.h>
#include <pmd.h>
#include <udp.h>
#include <utils.h>
#include <worker.h>

#include <csignal>
#include <iostream>
#include <optional>
#include <vector>

#include "ttime.h"

// The packets we send carry an ethernet, IPv4 and UDP header.
// The payload includes at least two 64-bit unsigned integers. The first one is
// used as a sequence number (0-indexed), and the second is the timestamp at
// packet creation (TSC).
constexpr uint16_t kMinPacketLength =
    std::max(sizeof(juggler::net::Ethernet) + sizeof(juggler::net::Ipv4) +
                 sizeof(juggler::net::Udp) + 2 * sizeof(uint64_t),
             64ul);

DEFINE_uint32(pkt_size, kMinPacketLength, "Packet size.");
DEFINE_uint64(tx_batch_size, 32, "DPDK TX packet batch size.");
DEFINE_double(drop_rate, 0, "Drop rate in forward drop mode in percent.");
DEFINE_string(
    config_json, "../src/apps/machnet/config.json",
    "Machnet JSON configuration file (shared with the pktgen application).");
DEFINE_string(remote_ip, "", "IPv4 address of the remote server.");
DEFINE_string(sremote_ip, "", "Second IPv4 address of the remote server.");
DEFINE_bool(forward_drop, true, "forward packets with a drop rate, default drop
rate is 0%. Otherwise it will reorder packets."); DEFINE_bool(zerocopy, true,
"Use memcpy to fill packet payload.");
DEFINE_string(rtt_log, "", "Log file for RTT measurements.");

// This is the source/destination UDP port used by the application.
const uint16_t kAppUDPPort = 6666;

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
   * @param remote_mac (std::optional) Remote host's MAC address in
   * `juggler::net::Ethernet::Address` format. `std::nullopt` in passive mode.
   * @param sremote_mac (std::optional) Second remote host's MAC address in
   * `juggler::net::Ethernet::Address` format. `std::nullopt` in passive mode.
   * @param remote_ip (std::optional) Remote host's IP address to be used by the
   * applicaton in `juggler::net::Ipv4::Address` format. `std::nullopt` in
   * passive mode.
   * @param sremote_ip (std::optional) Second remote host's IP address to be used by the
   * applicaton in `juggler::net::Ipv4::Address` format. `std::nullopt` in
   * passive mode.
   * @param drop_rate The rate at which packets are dropped in forward drop mode.
   * @param packet_size Size of the packets to be generated.
   * @param rxring Pointer to the RX ring previously initialized.
   * @param txring Pointer to the TX ring previously initialized.
   */
  task_context(juggler::net::Ethernet::Address local_mac,
               juggler::net::Ipv4::Address local_ip,
               std::optional<juggler::net::Ethernet::Address> remote_mac,
               std::optional<juggler::net::Ethernet::Address> sremote_mac,
               std::optional<juggler::net::Ipv4::Address> remote_ip,
               std::optional<juggler::net::Ipv4::Address> sremote_ip,
               uint16_t packet_size, double drop_rate,
               juggler::dpdk::RxRing *rxring, juggler::dpdk::TxRing *txring,
               std::vector<std::vector<uint8_t>> payloads)
      : local_mac_addr(local_mac),
        local_ipv4_addr(local_ip),
        remote_mac_addr(remote_mac),
        sremote_mac_addr(sremote_mac),
        remote_ipv4_addr(remote_ip),
        sremote_ipv4_addr(sremote_ip),
        packet_size(packet_size),
        drop_rate(drop_rate),
        rx_ring(CHECK_NOTNULL(rxring)),
        tx_ring(CHECK_NOTNULL(txring)),
        packet_payloads(payloads),
        arp_handler(local_mac, {local_ip}),
        statistics(),
        rtt_log() {}
  const juggler::net::Ethernet::Address local_mac_addr;
  const juggler::net::Ipv4::Address local_ipv4_addr;
  const std::optional<juggler::net::Ethernet::Address> remote_mac_addr;
  const std::optional<juggler::net::Ethernet::Address> sremote_mac_addr;
  const std::optional<juggler::net::Ipv4::Address> remote_ipv4_addr;
  const std::optional<juggler::net::Ipv4::Address> sremote_ipv4_addr;
  const uint16_t packet_size;
  const double drop_rate;

  juggler::dpdk::PacketPool *packet_pool;
  juggler::dpdk::RxRing *rx_ring;
  juggler::dpdk::TxRing *tx_ring;

  std::vector<std::vector<uint8_t>> packet_payloads;
  juggler::ArpHandler arp_handler;

  stats statistics;
  juggler::utils::TimeLog rtt_log;
  uint64_t packet_counter = {0};
};

/**
 * @brief Resolves the MAC address of a remote IP using ARP, with busy-waiting.
 *
 * This function sends an ARP request to resolve the MAC address of a given
 * remote IP address and waits for a response for the provided timeout duration.
 * If the MAC address is successfully resolved within the timeout, it is
 * returned; otherwise, a nullopt is returned.
 *
 * @param local_mac Local Ethernet MAC address.
 * @param local_ip Local IPv4 address.
 * @param tx_ring Pointer to the DPDK transmit ring.
 * @param rx_ring Pointer to the DPDK receive ring.
 * @param remote_ip Remote IPv4 address whose MAC address needs to be resolved.
 * @param timeout_in_sec Maximum time in seconds to wait for the ARP response.
 *
 * @return MAC address of the remote IP if successfully resolved, otherwise
 * returns nullopt.
 */
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
  LOG(INFO) << "RTT (ns) mean= " << mean << ", stddev=" << stddev
            << ", p50=" << p50 << ", p99=" << p99 << ", p999=" << p999
            << ", min=" << min << ", max=" << max;
  LOG(INFO) << juggler::utils::Format(
      "Application Statistics (TOTAL) - [TX] Sent: %lu, Drops: %lu, "
      "DropsNoMbuf: %lu "
      "[RX] Received: %lu",
      st->tx_success, st->err_tx_drops, st->err_no_mbufs, st->rx_count);
}

// Main network bounce drop routine.
// This routine acts as a middle box and can drop some potential packets between
// two machines receives packets, and bounces them back to the remote host.
void forward_drop(void *context) {
  auto ctx = static_cast<task_context *>(context);
  auto rx = ctx->rx_ring;
  auto tx = ctx->tx_ring;
  auto *st = &ctx->statistics;
  // NOTE: In bouncing mode we use only one packet pool for both RX and TX. In
  // particular, we use the pool that is associated with the RX ring. We only
  // need to touch the ethernet and IP headers. Since we don't use packets from
  // both pools this approach is also safe with `FAST_FREE' enabled.

  juggler::dpdk::PacketBatch rx_batch;
  auto packets_received = rx->RecvPackets(&rx_batch);
  std::array<size_t, juggler::dpdk::PacketBatch::kMaxBurst> tx_bytes;
  juggler::dpdk::PacketBatch tx_batch;
  for (uint16_t i = 0; i < rx_batch.GetSize(); i++) {
    auto *packet = rx_batch[i];
    CHECK_LE(sizeof(juggler::net::Ethernet), packet->length());

    packet->offload_udpv4_csum();

    auto *eh = packet->head_data<juggler::net::Ethernet *>();

    // Check if the packet is an ARP request and process it.
    if (eh->eth_type.value() == juggler::net::Ethernet::kArp) {
      const auto *arph = packet->head_data<juggler::net::Arp *>(
          sizeof(juggler::net::Ethernet));
      ctx->arp_handler.ProcessArpPacket(tx, arph);
      // We are going to drop this packet, so we need to explicitly reclaim the
      // mbuf now.
      juggler::dpdk::Packet::Free(packet);
      continue;
    }

    ctx->packet_counter++;

    if (ctx->drop_rate && ctx->packet_counter > 1 / (ctx->drop_rate / 100)) {
      ctx->packet_counter = 0;
      juggler::dpdk::Packet::Free(packet);
      continue;
    }

    auto *ipv4h = packet->head_data<juggler::net::Ipv4 *>(sizeof(*eh));

    // check if src addr is equal to ctx sremote ip
    if (ipv4h->src_addr.address == ctx->sremote_ipv4_addr.value().address) {
      ipv4h->dst_addr.address = ctx->remote_ipv4_addr.value().address;
      juggler::utils::Copy(&eh->dst_addr, &ctx->remote_mac_addr.value(),
                           sizeof(eh->dst_addr));
      // copy ctx remote mac addr to dst addr in eh header

    } else {
      ipv4h->dst_addr.address =
          juggler::be32_t(ctx->sremote_ipv4_addr.value().address);
      juggler::utils::Copy(&eh->dst_addr, &ctx->sremote_mac_addr.value(),
                           sizeof(eh->dst_addr));
    }

    ipv4h->src_addr.address = juggler::be32_t(ctx->local_ipv4_addr.address);
    juggler::utils::Copy(&eh->src_addr, &ctx->local_mac_addr,
                         sizeof(eh->src_addr));

    // Add the packet to the TX batch.
    tx_batch.Append(packet);

    const auto tx_bytes_index = tx_batch.GetSize() - 1;
    st->rx_bytes += packet->length();
    tx_bytes[tx_bytes_index] = packet->length();
    if (tx_bytes_index != 0)
      tx_bytes[tx_bytes_index] += tx_bytes[tx_bytes_index - 1];
  }
  rx_batch.Clear();

  auto packets_sent = tx->TrySendPackets(&tx_batch);
  st->err_tx_drops += packets_received - packets_sent;
  st->tx_success += packets_sent;
  if (packets_sent) st->tx_bytes += tx_bytes[packets_sent - 1];
  st->rx_count += packets_received;
}

// TODO: Alireza: Main network packet reorder routine.
// This routine acts as a middle box and can reorder some potential packets
// between two machines receives packets. It is currently just forwards packets
void packet_reorder(void *context) {
  auto ctx = static_cast<task_context *>(context);
  auto rx = ctx->rx_ring;
  auto tx = ctx->tx_ring;
  auto *st = &ctx->statistics;
  // NOTE: In bouncing mode we use only one packet pool for both RX and TX. In
  // particular, we use the pool that is associated with the RX ring. We only
  // need to touch the ethernet and IP headers. Since we don't use packets from
  // both pools this approach is also safe with `FAST_FREE' enabled.

  juggler::dpdk::PacketBatch rx_batch;
  auto packets_received = rx->RecvPackets(&rx_batch);
  std::array<size_t, juggler::dpdk::PacketBatch::kMaxBurst> tx_bytes;
  juggler::dpdk::PacketBatch tx_batch;
  for (uint16_t i = 0; i < rx_batch.GetSize(); i++) {
    auto *packet = rx_batch[i];
    CHECK_LE(sizeof(juggler::net::Ethernet), packet->length());

    packet->offload_udpv4_csum();

    auto *eh = packet->head_data<juggler::net::Ethernet *>();

    // Check if the packet is an ARP request and process it.
    if (eh->eth_type.value() == juggler::net::Ethernet::kArp) {
      const auto *arph = packet->head_data<juggler::net::Arp *>(
          sizeof(juggler::net::Ethernet));
      ctx->arp_handler.ProcessArpPacket(tx, arph);
      // We are going to drop this packet, so we need to explicitly reclaim the
      // mbuf now.
      juggler::dpdk::Packet::Free(packet);
      continue;
    }

    auto *ipv4h = packet->head_data<juggler::net::Ipv4 *>(sizeof(*eh));

    // check if src addr is equal to ctx sremote ip
    if (ipv4h->src_addr.address == ctx->sremote_ipv4_addr.value().address) {
      ipv4h->dst_addr.address = ctx->remote_ipv4_addr.value().address;
      juggler::utils::Copy(&eh->dst_addr, &ctx->remote_mac_addr.value(),
                           sizeof(eh->dst_addr));
      // copy ctx remote mac addr to dst addr in eh header

    } else {
      ipv4h->dst_addr.address =
          juggler::be32_t(ctx->sremote_ipv4_addr.value().address);
      juggler::utils::Copy(&eh->dst_addr, &ctx->sremote_mac_addr.value(),
                           sizeof(eh->dst_addr));
    }

    ipv4h->src_addr.address = juggler::be32_t(ctx->local_ipv4_addr.address);
    juggler::utils::Copy(&eh->src_addr, &ctx->local_mac_addr,
                         sizeof(eh->src_addr));

    // Add the packet to the TX batch.
    tx_batch.Append(packet);

    const auto tx_bytes_index = tx_batch.GetSize() - 1;
    st->rx_bytes += packet->length();
    tx_bytes[tx_bytes_index] = packet->length();
    if (tx_bytes_index != 0)
      tx_bytes[tx_bytes_index] += tx_bytes[tx_bytes_index - 1];
  }
  rx_batch.Clear();

  auto packets_sent = tx->TrySendPackets(&tx_batch);
  st->err_tx_drops += packets_received - packets_sent;
  st->tx_success += packets_sent;
  if (packets_sent) st->tx_bytes += tx_bytes[packets_sent - 1];
  st->rx_count += packets_received;
}

int main(int argc, char *argv[]) {
  ::google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  gflags::SetUsageMessage("Simple packet generator.");

  signal(SIGINT, int_handler);

  // Parse the remote IP address.
  std::optional<juggler::net::Ipv4::Address> remote_ip;
  std::optional<juggler::net::Ipv4::Address> sremote_ip;
  if (FLAGS_remote_ip.empty() || FLAGS_sremote_ip.empty()) {
    LOG(ERROR) << "Remote IP address is required in bounce drop mode.";
    exit(1);
  }
  remote_ip = juggler::net::Ipv4::Address::MakeAddress(FLAGS_remote_ip);
  sremote_ip = juggler::net::Ipv4::Address::MakeAddress(FLAGS_sremote_ip);
  CHECK(remote_ip.has_value())
      << "Invalid remote IP address: " << FLAGS_remote_ip;
  CHECK(sremote_ip.has_value())
      << "Invalid remote IP address: " << FLAGS_sremote_ip;

  // Load the configuration file.
  juggler::MachnetConfigProcessor machnet_config(FLAGS_config_json);
  if (machnet_config.interfaces_config().size() != 1) {
    // For machnet config, we expect only one interface to be configured.
    LOG(ERROR) << "Only one interface should be configured.";
    exit(1);
  }

  const auto &interface = *machnet_config.interfaces_config().begin();

  juggler::dpdk::Dpdk dpdk_obj;
  dpdk_obj.InitDpdk(machnet_config.GetEalOpts());

  const auto pmd_port_id = dpdk_obj.GetPmdPortIdByMac(interface.l2_addr());
  if (!pmd_port_id.has_value()) {
    LOG(ERROR) << "Failed to find DPDK port ID for MAC address: "
               << interface.l2_addr().ToString();
    exit(1);
  }

  juggler::dpdk::PmdPort pmd_obj(pmd_port_id.value());
  pmd_obj.InitDriver();

  auto *rxring = pmd_obj.GetRing<juggler::dpdk::RxRing>(0);
  CHECK_NOTNULL(rxring);
  auto *txring = pmd_obj.GetRing<juggler::dpdk::TxRing>(0);
  CHECK_NOTNULL(txring);

  // Resolve the remote host's MAC address if we are in active mode.
  std::optional<juggler::net::Ethernet::Address> remote_l2_addr = std::nullopt;
  if (!FLAGS_remote_ip.empty()) {
    remote_l2_addr =
        ArpResolveBusyWait(interface.l2_addr(), interface.ip_addr(), txring,
                           rxring, remote_ip.value(), 5);

    if (!remote_l2_addr.has_value()) {
      LOG(ERROR) << "Failed to resolve remote host's MAC address.";
      exit(1);
    }
  }

  // Resolve the second remote host's MAC address if we are in active mode.
  std::optional<juggler::net::Ethernet::Address> sremote_l2_addr = std::nullopt;
  if (!FLAGS_sremote_ip.empty()) {
    sremote_l2_addr =
        ArpResolveBusyWait(interface.l2_addr(), interface.ip_addr(), txring,
                           rxring, sremote_ip.value(), 5);

    if (!sremote_l2_addr.has_value()) {
      LOG(ERROR) << "Failed to resolve second remote host's MAC address.";
      exit(1);
    }
  }

  const uint16_t packet_len =
      std::max(std::min(static_cast<uint16_t>(FLAGS_pkt_size),
                        juggler::dpdk::PmdRing::kDefaultFrameSize),
               kMinPacketLength);

  std::vector<std::vector<uint8_t>> packet_payloads;
  if (!FLAGS_zerocopy) {
    const size_t kMaxPayloadMemory = 1 << 30;
    auto packet_payloads_nr = kMaxPayloadMemory / packet_len;
    // Round up to the closest power of 2.
    packet_payloads_nr = 1 << (64 - __builtin_clzll(packet_payloads_nr - 1));

    LOG(INFO) << "Allocating " << packet_payloads_nr
              << " payload buffers. (total: " << packet_payloads_nr * packet_len
              << " bytes)";
    packet_payloads.resize(packet_payloads_nr);
    for (auto &payload : packet_payloads) {
      payload.resize(packet_len);
    }
  }

  // auto tx_packet_pool = std::make_unique<juggler::dpdk::PacketPool>(4096);
  // We share the packet pool attached to the RX ring. Since we plan to handle a
  // queue pair from a single core this is safe.
  task_context task_ctx(interface.l2_addr(), interface.ip_addr(),
                        remote_l2_addr, sremote_l2_addr, remote_ip, sremote_ip,
                        packet_len, FLAGS_drop_rate, rxring, txring,
                        packet_payloads);

  auto packet_forward_drop_routine = [](uint64_t now, void *context) {
    forward_drop(context);
    report_stats(now, context);
  };

  auto packet_reorder_routine = [](uint64_t now, void *context) {
    packet_reorder(context);
    report_stats(now, context);
  };

  auto routine =
      FLAGS_forward_drop ? packet_forward_drop_routine : packet_reorder_routine;

  // Create a task object to pass to worker thread.
  auto task =
      std::make_shared<juggler::Task>(routine, static_cast<void *>(&task_ctx));

  if (FLAGS_forward_drop)
    std::cout << "Starting in passive message bouncing mode with drop."
              << std::endl;
  else
    std::cout << "Starting in passive message bouncing mode." << std::endl;

  juggler::WorkerPool<juggler::Task> WPool({task}, {interface.cpu_mask()});
  WPool.Init();

  // Set worker to running.
  WPool.Launch();

  while (g_keep_running) {
    sleep(5);
  }

  WPool.Pause();
  WPool.Terminate();
  report_final_stats(&task_ctx);
  pmd_obj.DumpStats();

  return (0);
}
