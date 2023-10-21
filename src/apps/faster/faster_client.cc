/**
 * @file main.cc
 * @brief This application is a simple message generator that supports sending
 * and receiving network messages using Machnet.
 */

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <hdr/hdr_histogram.h>
#include <machnet.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <numeric>
#include <sstream>
#include <thread>

using std::chrono::duration_cast;
using std::chrono::high_resolution_clock;
using std::chrono::time_point;

DEFINE_string(local_ip, "", "IP of the local Machnet interface");
DEFINE_string(remote_ip, "", "IP of the remote server's Machnet interface");
DEFINE_uint32(remote_port, 888, "Remote port to connect to.");
DEFINE_uint32(tx_msg_size, 64,
              "Size of the message (request/response) to send.");
DEFINE_uint32(msg_window, 8, "Maximum number of messages in flight.");
DEFINE_uint64(msg_nr, UINT64_MAX, "Number of messages to send.");
DEFINE_uint64(max_key_range, 10000, "Maximum key range to se");
DEFINE_bool(active_generator, false,
            "When 'true' this host is generating the traffic, otherwise it is "
            "bouncing.");
DEFINE_bool(verify, false, "Verify payload of received messages.");

static volatile int g_keep_running = 1;

struct msg_hdr_t {
  uint64_t window_slot;
  uint64_t key;
  uint64_t value;
};

struct stats_t {
  stats_t()
      : tx_success(0), tx_bytes(0), rx_count(0), rx_bytes(0), err_tx_drops(0) {}
  uint64_t tx_success;
  uint64_t tx_bytes;
  uint64_t rx_count;
  uint64_t rx_bytes;
  uint64_t err_tx_drops;
};

class ThreadCtx {
 private:
  static constexpr int64_t kMinLatencyMicros = 1;
  static constexpr int64_t kMaxLatencyMicros = 1000 * 1000 * 100;  // 100 sec
  static constexpr int64_t kLatencyPrecision = 2;  // Two significant digits

  struct msg_latency_info_t {
    time_point<high_resolution_clock> tx_ts;
  };

 public:
  ThreadCtx(const void *channel_ctx, const MachnetFlow_t *flow_info)
      : channel_ctx(CHECK_NOTNULL(channel_ctx)),
        flow(flow_info),
        key_start(0),
        single_key_counter(0),
        stats() {
    // Fill-in max-sized messages, we'll send the actual size later
    rx_message.resize(sizeof(msg_hdr_t));
    tx_message.resize(sizeof(msg_hdr_t));
    message_gold.resize(sizeof(msg_hdr_t));
    std::iota(message_gold.begin(), message_gold.end(), 0);
    std::memcpy(tx_message.data(), message_gold.data(), tx_message.size());

    int ret = hdr_init(kMinLatencyMicros, kMaxLatencyMicros, kLatencyPrecision,
                       &latency_hist);
    CHECK_EQ(ret, 0) << "Failed to initialize latency histogram.";

    msg_latency_info_vec.resize(FLAGS_msg_window);
  }
  ~ThreadCtx() { hdr_close(latency_hist); }

  void RecordRequestStart(uint64_t window_slot) {
    msg_latency_info_vec[window_slot].tx_ts = high_resolution_clock::now();
  }

  /// Return the request's latency in microseconds
  size_t RecordRequestEnd(uint64_t window_slot) {
    auto &msg_latency_info = msg_latency_info_vec[window_slot];
    auto latency_us = duration_cast<std::chrono::microseconds>(
                          high_resolution_clock::now() - msg_latency_info.tx_ts)
                          .count();

    hdr_record_value(latency_hist, latency_us);
    num_request_latency_samples++;
    return latency_us;
  }

 public:
  const void *channel_ctx;
  const MachnetFlow_t *flow;
  std::vector<uint8_t> rx_message;
  std::vector<uint8_t> tx_message;
  std::vector<uint8_t> message_gold;
  hdr_histogram *latency_hist;
  size_t num_request_latency_samples;
  std::vector<msg_latency_info_t> msg_latency_info_vec;
  size_t key_start;
  size_t single_key_counter;

  struct {
    stats_t current;
    stats_t prev;
    time_point<high_resolution_clock> last_measure_time;
  } stats;
};

void SigIntHandler([[maybe_unused]] int signal) { g_keep_running = 0; }

void ReportStats(ThreadCtx *thread_ctx) {
  auto now = high_resolution_clock::now();
  const double sec_elapsed = duration_cast<std::chrono::nanoseconds>(
                                 now - thread_ctx->stats.last_measure_time)
                                 .count() *
                             1.0 / 1E9;

  auto &cur = thread_ctx->stats.current;
  auto &prev = thread_ctx->stats.prev;

  if (sec_elapsed >= 1) {
    auto msg_sent = cur.tx_success - prev.tx_success;
    auto tx_mps = msg_sent / sec_elapsed;
    auto tx_gbps = ((cur.tx_bytes - prev.tx_bytes) * 8) / (sec_elapsed * 1E9);

    auto msg_received = cur.rx_count - prev.rx_count;
    auto rx_mps = msg_received / sec_elapsed;
    auto rx_gbps = ((cur.rx_bytes - prev.rx_bytes) * 8) / (sec_elapsed * 1E9);

    auto msg_dropped = cur.err_tx_drops - prev.err_tx_drops;
    auto tx_drop_mps = msg_dropped / sec_elapsed;

    std::ostringstream latency_stats_ss;
    latency_stats_ss << "Latency (us): ";
    if (thread_ctx->num_request_latency_samples > 0) {
      auto perc = hdr_value_at_percentile;
      latency_stats_ss << "median " << perc(thread_ctx->latency_hist, 50.0)
                       << ", 99th: " << perc(thread_ctx->latency_hist, 99.0)
                       << ", 99.9th: " << perc(thread_ctx->latency_hist, 99.9);
    } else {
      latency_stats_ss << "N/A";
    }

    LOG(INFO) << "TX MPS: " << tx_mps << " (" << tx_gbps
              << " Gbps), RX MPS: " << rx_mps << " (" << rx_gbps
              << " Gbps), TX_DROP MPS: " << tx_drop_mps << ", "
              << latency_stats_ss.str();

    thread_ctx->stats.last_measure_time = now;
    thread_ctx->stats.prev = thread_ctx->stats.current;
  }
}


void ClientSendOne(ThreadCtx *thread_ctx, uint64_t window_slot) {
  VLOG(1) << "Client: Sending message for window slot " << window_slot;
  auto &stats_cur = thread_ctx->stats.current;
  thread_ctx->msg_latency_info_vec[window_slot].tx_ts =
      high_resolution_clock::now();

  msg_hdr_t *msg_hdr =
      reinterpret_cast<msg_hdr_t *>(thread_ctx->tx_message.data());
  msg_hdr->window_slot = window_slot;
  if (thread_ctx->single_key_counter == 8) {
    thread_ctx->single_key_counter = 0;
    msg_hdr->key = thread_ctx->key_start;
  } else {
    thread_ctx->single_key_counter++;
    msg_hdr->key = thread_ctx->key_start++ % FLAGS_max_key_range;
  }

  msg_hdr->value = 0;

  const int ret =
      machnet_send(thread_ctx->channel_ctx, *thread_ctx->flow,
                   thread_ctx->tx_message.data(), sizeof(msg_hdr_t));
  if (ret == 0) {
    stats_cur.tx_success++;
    stats_cur.tx_bytes += sizeof(msg_hdr_t);
  } else {
    LOG(WARNING) << "Client: Failed to send message for window slot "
                 << window_slot;
    stats_cur.err_tx_drops++;
  }
}

// Return the window slot for which a response was received
uint64_t ClientRecvOneBlocking(ThreadCtx *thread_ctx) {
  const auto *channel_ctx = thread_ctx->channel_ctx;

  while (true) {
    if (g_keep_running == 0) {
      LOG(INFO) << "ClientRecvOneBlocking: Exiting.";
      return 0;
    }

    MachnetFlow_t rx_flow;
    const ssize_t rx_size =
        machnet_recv(channel_ctx, thread_ctx->rx_message.data(),
                     thread_ctx->rx_message.size(), &rx_flow);
    if (rx_size <= 0) continue;

    thread_ctx->stats.current.rx_count++;
    thread_ctx->stats.current.rx_bytes += rx_size;

    const auto *msg_hdr =
        reinterpret_cast<msg_hdr_t *>(thread_ctx->rx_message.data());
    if (msg_hdr->window_slot >= FLAGS_msg_window) {
      LOG(ERROR) << "Received invalid window slot: " << msg_hdr->window_slot;
      continue;
    }

    const size_t latency_us =
        thread_ctx->RecordRequestEnd(msg_hdr->window_slot);
    VLOG(1) << "Client: Received message for window slot "
            << msg_hdr->window_slot << " in " << latency_us << " us";

    if (FLAGS_verify) {
      if (msg_hdr->value != 0)
        CHECK(msg_hdr->key == msg_hdr->value);
    }

    return msg_hdr->window_slot;
  }

  LOG(FATAL) << "Should not reach here";
  return 0;
}

void ClientLoop(void *channel_ctx, MachnetFlow *flow) {
  ThreadCtx thread_ctx(channel_ctx, flow);
  LOG(INFO) << "Client Loop: Starting.";

  // Send a full window of messages
  for (uint32_t i = 0; i < FLAGS_msg_window; i++) {
    ClientSendOne(&thread_ctx, i /* window slot */);
  }

  while (true) {
    if (g_keep_running == 0) {
      LOG(INFO) << "MsgGenLoop: Exiting.";
      break;
    }

    const uint64_t rx_window_slot = ClientRecvOneBlocking(&thread_ctx);
    ClientSendOne(&thread_ctx, rx_window_slot);

    ReportStats(&thread_ctx);
  }

  auto &stats_cur = thread_ctx.stats.current;
  LOG(INFO) << "Application Statistics (TOTAL) - [TX] Sent: "
            << stats_cur.tx_success << " (" << stats_cur.tx_bytes
            << " Bytes), Drops: " << stats_cur.err_tx_drops
            << ", [RX] Received: " << stats_cur.rx_count << " ("
            << stats_cur.rx_bytes << " Bytes)";
}

int main(int argc, char *argv[]) {
  ::google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  gflags::SetUsageMessage("Simple Machnet-based message generator.");
  signal(SIGINT, SigIntHandler);

  CHECK_GT(FLAGS_tx_msg_size, sizeof(msg_hdr_t)) << "Message size too small";
  if (!FLAGS_active_generator) {
    LOG(INFO) << "Starting in server mode, response size " << FLAGS_tx_msg_size;
  } else {
    LOG(INFO) << "Starting in client mode, request size " << FLAGS_tx_msg_size;
  }

  CHECK_EQ(machnet_init(), 0) << "Failed to initialize Machnet library.";
  void *channel_ctx = machnet_attach();
  CHECK_NOTNULL(channel_ctx);

  MachnetFlow_t flow;
  if (FLAGS_active_generator) {
    int ret =
        machnet_connect(channel_ctx, FLAGS_local_ip.c_str(),
                        FLAGS_remote_ip.c_str(), FLAGS_remote_port, &flow);
    CHECK(ret == 0) << "Failed to connect to remote host. machnet_connect() "
                       "error: "
                    << strerror(ret);

    LOG(INFO) << "[CONNECTED] [" << FLAGS_local_ip << ":" << flow.src_port
              << " <-> " << FLAGS_remote_ip << ":" << flow.dst_port << "]";
  } else {
    int ret =
        machnet_listen(channel_ctx, FLAGS_local_ip.c_str(), FLAGS_remote_port);
    CHECK(ret == 0)
        << "Failed to listen on local port. machnet_listen() error: "
        << strerror(ret);

    LOG(INFO) << "[LISTENING] [" << FLAGS_local_ip << ":" << FLAGS_remote_port
              << "]";
  }

  std::thread datapath_thread;
  if (FLAGS_active_generator) {
    datapath_thread = std::thread(ClientLoop, channel_ctx, &flow);
  }

  while (g_keep_running) sleep(5);
  datapath_thread.join();
  return 0;
}
