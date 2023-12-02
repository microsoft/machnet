/**
 * @file main.cc
 * @brief This application is a simple message generator that supports sending
 * and receiving network messages using Linux Kernel TCP.
 */

#include <arpa/inet.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <hdr/hdr_histogram.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/tcp.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <numeric>
#include <sstream>
#include <thread>

#include "core/faster.h"
#include "device/null_disk.h"
#include "test_types.h"

// #define MSG_MAX_LEN (8 * ((1 << 10) * (1 << 10)))

#define MSG_MAX_LEN 1024

using std::chrono::duration_cast;
using std::chrono::high_resolution_clock;
using std::chrono::time_point;

using namespace FASTER::core;
using FASTER::test::FixedSizeKey;
using FASTER::test::NonCopyable;
using FASTER::test::NonMovable;
using FASTER::test::SimpleAtomicValue;

DEFINE_string(local_ip, "", "IP of the local interface");
DEFINE_string(remote_ip, "", "IP of the remote server's interface");
DEFINE_uint32(port, 888, "Port to listen.");
DEFINE_uint32(tx_msg_size, 64,
              "Size of the message (request/response) to send.");
DEFINE_uint32(msg_window, 8, "Maximum number of messages in flight.");
DEFINE_uint64(msg_nr, UINT64_MAX, "Number of messages to send.");
DEFINE_uint64(num_keys, 10000, "Number of keys to use.");
DEFINE_bool(active_generator, false,
            "When 'true' this host is generating the traffic, otherwise it is "
            "bouncing.");
DEFINE_bool(verify, false, "Verify payload of received messages.");
DEFINE_uint32(num_threads, 1, "Maximum number of threasds to use.");

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
  explicit ThreadCtx(int sock_fd) : sock_fd(sock_fd), stats(), key(0) {
    // Fill-in max-sized messages, we'll send the actual size later
    rx_message.resize(MSG_MAX_LEN);
    tx_message.resize(MSG_MAX_LEN);
    message_gold.resize(MSG_MAX_LEN);
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
  std::vector<uint8_t> rx_message;
  std::vector<uint8_t> tx_message;
  std::vector<uint8_t> message_gold;
  hdr_histogram *latency_hist{};
  int sock_fd;
  size_t num_request_latency_samples{};
  size_t num_warmup_requests{};
  std::vector<msg_latency_info_t> msg_latency_info_vec;

  struct {
    stats_t current;
    stats_t prev;
    time_point<high_resolution_clock> last_measure_time;
  } stats;

  uint64_t key;
};

void SigIntHandler([[maybe_unused]] int signal) { g_keep_running = 0;  }

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

class Latch {
 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool triggered_ = false;

 public:
  void Wait() {
    std::unique_lock<std::mutex> lock{mutex_};
    while (!triggered_) {
      cv_.wait(lock);
    }
  }

  void Trigger() {
    std::unique_lock<std::mutex> lock{mutex_};
    triggered_ = true;
    cv_.notify_all();
  }

  void Reset() { triggered_ = false; }
};

template <typename Callable, typename... Args>
void run_threads(size_t num_threads, Callable worker, Args... args) {
  Latch latch;
  auto run_thread = [&latch, &worker, &args...](size_t idx) {
    latch.Wait();
    worker(idx, args...);
  };

  std::deque<std::thread> threads{};
  for (size_t idx = 0; idx < num_threads; ++idx) {
    threads.emplace_back(run_thread, idx);
  }

  latch.Trigger();
  for (auto& thread : threads) {
    thread.join();
  }
}

using Key = FixedSizeKey<uint64_t>;
using Value = SimpleAtomicValue<uint64_t>;

class UpsertContext : public IAsyncContext {
 public:
  typedef Key key_t;
  typedef Value value_t;

  UpsertContext(uint64_t key, uint64_t val) : key_{key}, val_{val} {}

  /// Copy (and deep-copy) constructor.
  UpsertContext(const UpsertContext& other) : key_{other.key_} {}

  /// The implicit and explicit interfaces require a key() accessor.
  inline const Key& key() const { return key_; }
  inline static constexpr uint32_t value_size() { return sizeof(value_t); }
  /// Non-atomic and atomic Put() methods.
  inline void Put(Value& value) { value.value = val_.value; }
  inline bool PutAtomic(Value& value) {
    value.atomic_value.store(42);
    return true;
  }

 protected:
  /// The explicit interface requires a DeepCopy_Internal() implementation.
  Status DeepCopy_Internal(IAsyncContext*& context_copy) {
    return IAsyncContext::DeepCopy_Internal(*this, context_copy);
  }

 private:
  Key key_;
  Value val_;
};

class ReadContext : public IAsyncContext {
 public:
  typedef Key key_t;
  typedef Value value_t;

  ReadContext(uint64_t key) : key_{key} {}

  /// Copy (and deep-copy) constructor.
  ReadContext(const ReadContext& other) : key_{other.key_} {}

  /// The implicit and explicit interfaces require a key() accessor.
  inline const Key& key() const { return key_; }

  inline void Get(const Value& value) {
    // All reads should be atomic (from the mutable tail).
    CHECK_EQ(true, false);
  }
  inline void GetAtomic(const Value& value) {
    output = value.atomic_value.load();
  }

 protected:
  /// The explicit interface requires a DeepCopy_Internal() implementation.
  Status DeepCopy_Internal(IAsyncContext*& context_copy) {
    return IAsyncContext::DeepCopy_Internal(*this, context_copy);
  }

 private:
  Key key_;

 public:
  uint64_t output;
};

FasterKv<Key, Value, FASTER::device::NullDisk> store{1<<25, 1073741824, ""};

void ServerLoop(void *sock_fd) {
  ThreadCtx thread_ctx(*reinterpret_cast<int *>(sock_fd));

  LOG(INFO) << "Server Loop: Starting.";

  store.StartSession();

  auto callback = [](IAsyncContext *ctxt, Status result) {
    // It is an in-memory store so, we never get here!
    CHECK_EQ(true, false);
  };

  while (true) {
    if (g_keep_running == 0) {
      LOG(INFO) << "MsgGenLoop: Exiting.";
      break;
    }

    auto &stats_cur = thread_ctx.stats.current;

    const ssize_t rx_size = recv(thread_ctx.sock_fd, thread_ctx.rx_message.data(),
                                 thread_ctx.rx_message.size(), 0);

    // if (rx_size == 0) g_keep_running = 0;
    if (rx_size <= 0) continue;

    stats_cur.rx_count++;
    stats_cur.rx_bytes += rx_size;

    const msg_hdr_t *msg_hdr =
        reinterpret_cast<const msg_hdr_t *>(thread_ctx.rx_message.data());

    VLOG(1) << "Server: Received msg for window slot " << msg_hdr->window_slot
            << " key " << msg_hdr->key << " value " << msg_hdr->value;

    uint64_t key = msg_hdr->key;

    ReadContext context{msg_hdr->key};
    Status result = store.Read(context, callback, 1);

    // Send the response
    msg_hdr_t *tx_msg_hdr =
        reinterpret_cast<msg_hdr_t *>(thread_ctx.tx_message.data());
    tx_msg_hdr->window_slot = msg_hdr->window_slot;
    tx_msg_hdr->key = key;

     if (result == Status::Ok) {
      tx_msg_hdr->value = context.output;
    } else {
      tx_msg_hdr->value = 0;
      LOG(WARNING) << "Key not found";
    }

    const int ret = send(thread_ctx.sock_fd, thread_ctx.tx_message.data(),
                         sizeof(msg_hdr_t), 0);
    if (ret >= 0) {
      stats_cur.tx_success++;
      stats_cur.tx_bytes += ret;  // FLAGS_tx_msg_size;
    } else {
      stats_cur.err_tx_drops++;
    }

    ReportStats(&thread_ctx);
  }

  store.StopSession();

  auto &stats_cur = thread_ctx.stats.current;
  LOG(INFO) << "Application Statistics (TOTAL) - [TX] Sent: "
            << stats_cur.tx_success << " (" << stats_cur.tx_bytes
            << " Bytes), Drops: " << stats_cur.err_tx_drops
            << ", [RX] Received: " << stats_cur.rx_count << " ("
            << stats_cur.rx_bytes << " Bytes)";
}

void ClientSendOne(ThreadCtx *thread_ctx, uint64_t window_slot) {
  VLOG(1) << "Client: Sending message for window slot " << window_slot;
  auto &stats_cur = thread_ctx->stats.current;
  thread_ctx->msg_latency_info_vec[window_slot].tx_ts =
      high_resolution_clock::now();

  msg_hdr_t *msg_hdr =
      reinterpret_cast<msg_hdr_t *>(thread_ctx->tx_message.data());

  msg_hdr->window_slot = window_slot;
  msg_hdr->key = thread_ctx->key++ % FLAGS_num_keys;
  msg_hdr->value = 0;

  const int ret = write(thread_ctx->sock_fd, thread_ctx->tx_message.data(),
                        sizeof(msg_hdr_t));
  if (ret == sizeof(msg_hdr_t)) {
    stats_cur.tx_success++;
    stats_cur.tx_bytes += ret;
  } else {
    LOG(WARNING) << "Client: Failed to send message for window slot "
                 << window_slot << ". write() error: " << strerror(errno);
    stats_cur.err_tx_drops++;
  }
}


// Return the window slot for which a response was received
uint64_t ClientRecvOneRealBlocking(ThreadCtx *thread_ctx) {
  const ssize_t rx_size =
      read(thread_ctx->sock_fd, thread_ctx->rx_message.data(),
           thread_ctx->rx_message.size());

  thread_ctx->stats.current.rx_count++;
  thread_ctx->stats.current.rx_bytes += rx_size;

  const auto *msg_hdr =
      reinterpret_cast<msg_hdr_t *>(thread_ctx->rx_message.data());
  if (msg_hdr->window_slot >= FLAGS_msg_window) {
    LOG(ERROR) << "Received invalid window slot: " << msg_hdr->window_slot;
  }

  if (thread_ctx->num_warmup_requests++ > 10000) {
    const size_t latency_us =
        thread_ctx->RecordRequestEnd(msg_hdr->window_slot);
    VLOG(1) << "Client: Received message for window slot "
            << msg_hdr->window_slot << " in " << latency_us << " us";
  }

  if (FLAGS_verify) {
    for (uint32_t i = sizeof(msg_hdr_t); i < rx_size; i++) {
      if (thread_ctx->rx_message[i] != thread_ctx->message_gold[i]) {
        LOG(ERROR) << "Message data mismatch at index " << i << std::hex << " "
                   << static_cast<uint32_t>(thread_ctx->rx_message[i]) << " "
                   << static_cast<uint32_t>(thread_ctx->message_gold[i]);
        break;
      }
    }
  }

  return msg_hdr->window_slot;

  LOG(FATAL) << "Should not reach here";
}

// Return the window slot for which a response was received
uint64_t ClientRecvOneBlocking(ThreadCtx *thread_ctx) {
  while (true) {
    if (g_keep_running == 0) {
      LOG(INFO) << "ClientRecvOneBlocking: Exiting.";
      return 0;
    }

    const ssize_t rx_size = read(
        thread_ctx->sock_fd, thread_ctx->rx_message.data(), thread_ctx->rx_message.size());
    if (rx_size <= 0) continue;

    thread_ctx->stats.current.rx_count++;
    thread_ctx->stats.current.rx_bytes += rx_size;

    const auto *msg_hdr =
        reinterpret_cast<msg_hdr_t *>(thread_ctx->rx_message.data());
    if (msg_hdr->window_slot >= FLAGS_msg_window) {
      LOG(ERROR) << "Received invalid window slot: " << msg_hdr->window_slot;
      continue;
    }

    if (thread_ctx->num_warmup_requests++ > 10000) {
      const size_t latency_us =
          thread_ctx->RecordRequestEnd(msg_hdr->window_slot);
      VLOG(1) << "Client: Received message for window slot "
              << msg_hdr->window_slot << " in " << latency_us << " us";
    }

    if (FLAGS_verify) {
      for (uint32_t i = sizeof(msg_hdr_t); i < rx_size; i++) {
        if (thread_ctx->rx_message[i] != thread_ctx->message_gold[i]) {
          LOG(ERROR) << "Message data mismatch at index " << i << std::hex
                     << " " << static_cast<uint32_t>(thread_ctx->rx_message[i])
                     << " "
                     << static_cast<uint32_t>(thread_ctx->message_gold[i]);
          break;
        }
      }
    }

    return msg_hdr->window_slot;
  }

  LOG(FATAL) << "Should not reach here";
}

void ClientLoopBlockingSemantic(void *sock_fd) {
  ThreadCtx thread_ctx(*reinterpret_cast<int *>(sock_fd));
  LOG(INFO) << "Client Loop: Starting.";

  while (true) {
    if (g_keep_running == 0) {
      LOG(INFO) << "MsgGenLoop: Exiting.";
      break;
    }

    ClientSendOne(&thread_ctx, 0);

    ClientRecvOneRealBlocking(&thread_ctx);

    ReportStats(&thread_ctx);
  }

  auto &stats_cur = thread_ctx.stats.current;
  LOG(INFO) << "Application Statistics (TOTAL) - [TX] Sent: "
            << stats_cur.tx_success << " (" << stats_cur.tx_bytes
            << " Bytes), Drops: " << stats_cur.err_tx_drops
            << ", [RX] Received: " << stats_cur.rx_count << " ("
            << stats_cur.rx_bytes << " Bytes)";
}

void ClientLoop(void *sock_fd) {
  ThreadCtx thread_ctx(*reinterpret_cast<int *>(sock_fd));
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

void Populate() {

  store.StartSession();

  auto callback = [](IAsyncContext* ctxt, Status result) {
    CHECK_EQ(true, false);
  };

  LOG(INFO) << "Populating store with " << FLAGS_num_keys << " keys";

  for (size_t idx = 0; idx < FLAGS_num_keys; ++idx) {
    UpsertContext context{static_cast<uint64_t>(idx), static_cast<uint64_t>(idx)};
    Status result = store.Upsert(context, callback, 1);
    CHECK_EQ(Status::Ok, result);
    if (idx % 500000 == 0) {
      LOG(INFO) << "Inserted " << idx << " keys";
    }
  }

  LOG(INFO) << "Finished populating store";

  store.StopSession();
}

int main(int argc, char *argv[]) {
  ::google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  gflags::SetUsageMessage("Simple Linux Kernel TCP-based message generator.");
  signal(SIGINT, SigIntHandler);

  CHECK_GT(FLAGS_tx_msg_size, sizeof(msg_hdr_t)) << "Message size too small";
  if (!FLAGS_active_generator) {
    LOG(INFO) << "Starting in server mode, response size " << FLAGS_tx_msg_size;
  } else {
    LOG(INFO) << "Starting in client mode, request size " << FLAGS_tx_msg_size;
  }

  int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(sock_fd > 0) << "Failed to create a socket. socket() error: "
                     << strerror(sock_fd);
  sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;

  if (FLAGS_active_generator) {
    // client
    server_addr.sin_port = htons(FLAGS_port);
    int ret =
        inet_pton(AF_INET, FLAGS_remote_ip.c_str(), &server_addr.sin_addr);
    CHECK(ret == 1) << "Failed to resolve the remote ip.";

    // Set TCP no-delay option
    int enable = 1;
    if (setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &enable,
                   sizeof(int)) == -1) {
      perror("Error setting TCP no-delay");
      return 1;
    }

    ret = connect(sock_fd, reinterpret_cast<sockaddr *>(&server_addr),
                  sizeof(server_addr));
    CHECK(ret == 0) << "Failed to connect to remote host. connect() "
                       "error: "
                    << strerror(ret);



    LOG(INFO) << "[CONNECTED] [" << FLAGS_local_ip << " <-> " << FLAGS_remote_ip
              << ":" << FLAGS_port << "]";
  } else {
    // server
    int optval = 1;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval,
               sizeof(int))) {
      perror("Error setting TCP port as reusable");
      return 1;
    }

    // Set TCP no-delay option
    int enable = 1;
    if (setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &enable,
                   sizeof(int)) == -1) {
      perror("Error setting TCP no-delay");
      return 1;
    }

    server_addr.sin_port = htons(FLAGS_port);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    int ret = bind(sock_fd, reinterpret_cast<sockaddr *>(&server_addr),
                   sizeof(server_addr));
    CHECK(ret == 0) << "Failed to bind on fd: " << sock_fd
                    << " bind() error: " << strerror(errno);
    ret = listen(sock_fd, 10);
    CHECK(ret == 0) << "Failed to listen on fd: " << sock_fd
                    << " listen() error: " << strerror(errno);
    LOG(INFO) << "[LISTENING] [" << FLAGS_local_ip << ":" << FLAGS_port << "]";
  }

  std::vector<std::thread>datapath_threads;
  if (FLAGS_active_generator) {
    datapath_threads.emplace_back(ClientLoopBlockingSemantic, &sock_fd);
  } else {
    Populate();

    while (g_keep_running) {
      // Accept a new client connection

      sockaddr_in client_addr;
      socklen_t client_addr_size = sizeof(client_addr);
      int client_sock_fd =
          accept(sock_fd, reinterpret_cast<sockaddr *>(&client_addr),
                 &client_addr_size);
      CHECK(client_sock_fd > 0)
          << "Server: Failed to accept connection. accept():  "
          << strerror(errno);
      if (client_sock_fd == -1) {
        LOG(WARNING) << "Error accepting connection" << std::endl;
        continue;
      }

      LOG(INFO) << "New connection accepted" << std::endl;

      // Create a new thread to handle the client
      datapath_threads.emplace_back(ServerLoop, &client_sock_fd);
    }
  }

  while (g_keep_running) sleep(5);
  for (auto& thread : datapath_threads) {
    thread.join();
  }
  return 0;
}
