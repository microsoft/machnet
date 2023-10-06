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
DEFINE_uint64(tx_batch_size, 32, "DPDK TX packet batch size");
DEFINE_string(
    config_json, "../src/apps/machnet/config.json",
    "Machnet JSON configuration file (shared with the pktgen application).");
DEFINE_string(remote_ip, "", "IPv4 address of the remote server.");
DEFINE_bool(ping, false, "Ping-pong remote host for RTT measurements.");
DEFINE_bool(active_generator, false,
            "When 'true' this host is generating the traffic, otherwise it is "
            "bouncing.");
DEFINE_bool(zerocopy, true, "Use memcpy to fill packet payload.");
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
   * @param remote_ip (std::optional) Remote host's IP address to be used by the
   * applicaton in `juggler::net::Ipv4::Address` format. `std::nullopt` in
   * passive mode.
   * @param packet_size Size of the packets to be generated.
   * @param rxring Pointer to the RX ring previously initialized.
   * @param txring Pointer to the TX ring previously initialized.
   */
  task_context(juggler::net::Ethernet::Address local_mac,
               juggler::net::Ipv4::Address local_ip,
               std::optional<juggler::net::Ethernet::Address> remote_mac,
               std::optional<juggler::net::Ipv4::Address> remote_ip,
               uint16_t packet_size, juggler::dpdk::RxRing *rxring,
               juggler::dpdk::TxRing *txring,
               std::vector<std::vector<uint8_t>> payloads)
      : local_mac_addr(local_mac),
        local_ipv4_addr(local_ip),
        remote_mac_addr(remote_mac),
        remote_ipv4_addr(remote_ip),
        packet_size(packet_size),
        rx_ring(CHECK_NOTNULL(rxring)),
        tx_ring(CHECK_NOTNULL(txring)),
        packet_payloads(payloads),
        arp_handler(local_mac, {local_ip}),
        statistics(),
        rtt_log() {}
  const juggler::net::Ethernet::Address local_mac_addr;
  const juggler::net::Ipv4::Address local_ipv4_addr;
  const std::optional<juggler::net::Ethernet::Address> remote_mac_addr;
  const std::optional<juggler::net::Ipv4::Address> remote_ipv4_addr;
  const uint16_t packet_size;

  juggler::dpdk::PacketPool *packet_pool;
  juggler::dpdk::RxRing *rx_ring;
  juggler::dpdk::TxRing *tx_ring;

  std::vector<std::vector<uint8_t>> packet_payloads;
  juggler::ArpHandler arp_handler;

  stats statistics;
  juggler::utils::TimeLog rtt_log;
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
 * @brief Prepares a packet with L2, L3, and L4 headers and sets the provided
 * sequence number and timestamp.
 *
 * This function populates the Ethernet, IPv4, and UDP headers of the provided
 * packet. It then sets the sequence number and timestamp provided as arguments.
 * Optionally, it also sets the payload for the packet.
 *
 * @param context A pointer to the context containing the necessary packet
 * information.
 * @param packet A pointer to the packet to be prepared.
 * @param seqno The sequence number to set in the packet.
 * @param timestamp The timestamp to set in the packet.
 *
 * @note This function assumes the packet is large enough to contain all the
 * headers, the sequence number, the timestamp, and the optional payload;
 * otherwise, it will result in abort execution.
 */
void prepare_packet(void *context, juggler::dpdk::Packet *packet,
                    uint64_t seqno, uint64_t timestamp) {
  auto *ctx = static_cast<task_context *>(context);

  const auto len = ctx->packet_size;
  CHECK_NOTNULL(packet->append(len));

  // Prepare the L2 header.
  auto *eh = packet->head_data<juggler::net::Ethernet *>();
  eh->src_addr = ctx->local_mac_addr;
  eh->dst_addr = ctx->remote_mac_addr.value();
  eh->eth_type = juggler::be16_t(RTE_ETHER_TYPE_IPV4);

  // Prepare the L3 header.
  auto *ipv4h = reinterpret_cast<juggler::net::Ipv4 *>(eh + 1);
  ipv4h->version_ihl = 0x45;
  ipv4h->type_of_service = 0;
  ipv4h->packet_id = juggler::be16_t(0x1513);
  ipv4h->fragment_offset = juggler::be16_t(0);
  ipv4h->time_to_live = juggler::net::Ipv4::kDefaultTTL;
  ipv4h->next_proto_id = juggler::net::Ipv4::Proto::kUdp;
  ipv4h->total_length = juggler::be16_t(len - sizeof(*eh));
  ipv4h->src_addr.address = juggler::be32_t(ctx->local_ipv4_addr.address);
  ipv4h->dst_addr.address =
      juggler::be32_t(ctx->remote_ipv4_addr.value().address);
  ipv4h->hdr_checksum = 0;

  // Prepare the L4 header.
  auto *udph = reinterpret_cast<juggler::net::Udp *>(ipv4h + 1);
  udph->src_port.port = juggler::be16_t(kAppUDPPort);
  udph->dst_port.port = juggler::be16_t(kAppUDPPort);
  udph->len = juggler::be16_t(len - sizeof(*eh) - sizeof(*ipv4h));
  udph->cksum = juggler::be16_t(0);

  // Set the sequence number.
  auto *pkt_seqno = reinterpret_cast<uint64_t *>(udph + 1);
  *pkt_seqno = seqno++;

  // Set the timestamp.
  auto *pkt_timestamp = reinterpret_cast<uint64_t *>(pkt_seqno + 1);
  *pkt_timestamp = timestamp;

  // Set the payload.
  if (!FLAGS_zerocopy) {
    auto payload_len = len - kMinPacketLength;
    const size_t kPayloadArrayBitmask = ctx->packet_payloads.size() - 1;
    static size_t payload_idx = 0;
    auto *src_payload =
        ctx->packet_payloads[payload_idx & kPayloadArrayBitmask].data();
    auto *dst_payload = reinterpret_cast<uint8_t *>(pkt_timestamp + 1);
    juggler::utils::Copy(dst_payload, src_payload, payload_len);
    payload_idx++;
  }

  // Offload IPv4 and UDP checksums to hardware.
  packet->set_l2_len(sizeof(*eh));
  packet->set_l3_len(sizeof(*ipv4h));
  packet->offload_udpv4_csum();
}

/**
 * @brief This is a ping routine that sends a packet to the remote host and
 * measures the RTT.
 * @param now Current timestamp in nanoseconds.
 * @param context Opaque pointer to the task context.
 */
void ping(uint64_t now, void *context) {
  thread_local uint64_t last_ping_time;
  thread_local uint64_t last_report_time;
  auto *ctx = static_cast<task_context *>(context);
  auto *tx = ctx->tx_ring;
  auto *rx = ctx->rx_ring;
  auto *st = &ctx->statistics;

  if (now - last_ping_time >= juggler::time::ms_to_cycles(1)) {
    // Allocate a packet from the packet pool.
    auto *packet = tx->GetPacketPool()->PacketAlloc();
    if (packet == nullptr) {
      st->err_no_mbufs++;
      return;
    }

    // Prepare the packet.
    auto seqno = st->tx_success;
    now = juggler::time::rdtsc();
    prepare_packet(context, packet, seqno++, now);

    // Send the packet.
    tx->SendPackets(&packet, 1);
    st->tx_success++;
    st->tx_bytes += ctx->packet_size;
    last_ping_time = now;
  }

  juggler::dpdk::PacketBatch batch;
  auto packets_received = rx->RecvPackets(&batch);
  now = juggler::time::rdtsc();  // Update the timestamp.
  for (uint16_t i = 0; i < packets_received; i++) {
    auto *packet = batch[i];
    if (packet->length() != ctx->packet_size) continue;

    auto *eh = packet->head_data<juggler::net::Ethernet *>();
    auto *ipv4h = reinterpret_cast<juggler::net::Ipv4 *>(eh + 1);
    if (ipv4h->src_addr.address != ctx->remote_ipv4_addr.value().address)
      continue;

    // This is a valid `ping` response.
    auto *udph = reinterpret_cast<juggler::net::Udp *>(ipv4h + 1);
    auto *pkt_seqno = reinterpret_cast<uint64_t *>(udph + 1);

    auto *pkt_timestamp = reinterpret_cast<uint64_t *>(pkt_seqno + 1);

    // Calculate the number of intermediate hops.
    // Note: This is not a reliable way to calculate the number of hops,
    // especially in the cloud.
    auto num_hops = juggler::net::Ipv4::kDefaultTTL - ipv4h->time_to_live;

    // Calculate the round-trip time.
    const auto rtt = juggler::time::cycles_to_ns(now - *pkt_timestamp);
    ctx->rtt_log.Record(rtt);
    st->rx_count++;
    st->rx_bytes += packet->length();

    if (now - last_report_time >= juggler::time::ms_to_cycles(1000)) {
      // Report the RTT.
      VLOG(1) << "RTT(us): " << static_cast<double>(rtt) / 1E3
              << ", num_hops: " << num_hops;
      last_report_time = now;
    }
  }

  batch.Release();
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
  if (!FLAGS_ping) return;

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

// Main transmit (generator) routine.
// It generates packets in batches and attempts to transmit them to the remote
// host.
void tx(void *context) {
  auto *ctx = static_cast<task_context *>(context);
  auto *tx = ctx->tx_ring;
  auto *pp = tx->GetPacketPool();
  auto *st = &ctx->statistics;

  thread_local uint64_t seqno;
  juggler::dpdk::PacketBatch batch;
  const auto ret = pp->PacketBulkAlloc(&batch, FLAGS_tx_batch_size);
  if (!ret) {
    st->err_no_mbufs++;
    return;
  }

  for (uint16_t i = 0; i < batch.GetSize(); i++) {
    auto *packet = batch[i];
    prepare_packet(context, packet, seqno++, 0);
  }

  auto packets_sent = tx->TrySendPackets(&batch);
  st->err_tx_drops += FLAGS_tx_batch_size - packets_sent;
  st->tx_success += packets_sent;
  st->tx_bytes += static_cast<uint64_t>(packets_sent * ctx->packet_size);
}

// Main network receive routine.
// This function receives packets in the application, and immediately releases
// the mbufs.
void rx(void *context) {
  auto ctx = static_cast<task_context *>(context);
  auto *rx = ctx->rx_ring;
  auto *st = &ctx->statistics;

  juggler::dpdk::PacketBatch batch;
  auto packets_received = rx->RecvPackets(&batch);
  for (uint16_t i = 0; i < packets_received; i++) {
    auto *packet = batch[i];
    st->rx_bytes += packet->length();
  }
  st->rx_count += packets_received;
  // We need to release the received packet mbufs back to the pool.
  batch.Release();
}

// Main network bounce routine.
// This routine receives packets, and bounces them back to the remote host.
void bounce(void *context) {
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

    juggler::net::Ethernet::Address tmp;
    juggler::utils::Copy(&tmp, &eh->dst_addr, sizeof(tmp));
    juggler::utils::Copy(&eh->dst_addr, &eh->src_addr, sizeof(eh->dst_addr));
    juggler::utils::Copy(&eh->src_addr, &tmp, sizeof(eh->src_addr));

    auto *ipv4h = packet->head_data<juggler::net::Ipv4 *>(sizeof(*eh));
    ipv4h->dst_addr.address = ipv4h->src_addr.address;
    ipv4h->src_addr.address = juggler::be32_t(ctx->local_ipv4_addr.address);

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
  if (!FLAGS_active_generator && !FLAGS_ping) {
    // In passive mode, we don't need the remote IP address.
    remote_ip = std::nullopt;
  } else {
    if (FLAGS_remote_ip.empty()) {
      LOG(ERROR) << "Remote IP address is required in active mode.";
      exit(1);
    }
    remote_ip = juggler::net::Ipv4::Address::MakeAddress(FLAGS_remote_ip);
    CHECK(remote_ip.has_value())
        << "Invalid remote IP address: " << FLAGS_remote_ip;
  }

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
  if (FLAGS_active_generator || FLAGS_ping) {
    remote_l2_addr =
        ArpResolveBusyWait(interface.l2_addr(), interface.ip_addr(), txring,
                           rxring, remote_ip.value(), 5);
    if (!remote_l2_addr.has_value()) {
      LOG(ERROR) << "Failed to resolve remote host's MAC address.";
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
                        remote_l2_addr, remote_ip, packet_len, rxring, txring,
                        packet_payloads);

  auto packet_generator = [](uint64_t now, void *context) {
    tx(context);
    rx(context);
    report_stats(now, context);
  };

  auto pingpong = [](uint64_t now, void *context) { ping(now, context); };

  auto packet_bouncer = [](uint64_t now, void *context) {
    bounce(context);
    report_stats(now, context);
  };

  auto routine =
      FLAGS_ping ? pingpong
                 : (FLAGS_active_generator ? packet_generator : packet_bouncer);
  // Create a task object to pass to worker thread.
  auto task =
      std::make_shared<juggler::Task>(routine, static_cast<void *>(&task_ctx));

  if (FLAGS_ping)
    std::cout << "Starting in ping mode; press Ctrl-C to stop." << std::endl;
  else if (FLAGS_active_generator)
    std::cout << "Starting in active message generator mode." << std::endl;
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
