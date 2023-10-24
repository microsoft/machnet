#include <arpa/inet.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <hdr/hdr_histogram.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <numeric>
#include <sstream>
#include <thread>

using std::chrono::duration_cast;
using std::chrono::high_resolution_clock;
using std::chrono::time_point;

DEFINE_uint64(num_keys, 1000, "Number of keys to request for");
DEFINE_uint32(thread_size, 1,
              "Number of threads to use for the server application");
DEFINE_string(remote, "127.0.0.1",
              "Remote IP address, needed for Machnet only");
DEFINE_uint32(remote_port, 0, "Remote port");
DEFINE_bool(verify, false, "Verify the results");
DEFINE_uint64(msg_window, 1, "Number of messages in flight");

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
  ThreadCtx()
      : key_start(0),
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
//   const void *channel_ctx;
//   const MachnetFlow_t *flow;
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

int main(int argc, char* argv[]) {
  // Create a UDP socket

  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  FLAGS_logtostderr = 1;

  int udpSocket = socket(AF_INET, SOCK_DGRAM, 0);
  if (udpSocket == -1) {
    perror("Failed to create UDP socket");
    return 1;
  }

  // Define the server address (IP and port)
  sockaddr_in serverAddress;
  serverAddress.sin_family = AF_INET;
  serverAddress.sin_port = htons(FLAGS_remote_port);  // Port number
  serverAddress.sin_addr.s_addr = inet_addr(
      FLAGS_remote.c_str());  // Server IP address (change to the actual server's IP)

  uint64_t i = 0;
  while (i < FLAGS_num_keys) {
    const msg_hdr_t msg = {0, i, 15};

    // Send the message to the server
    ssize_t bytesSent =
        sendto(udpSocket, &msg, sizeof(msg), 0,
               (struct sockaddr*)&serverAddress, sizeof(serverAddress));

    if (bytesSent == -1) {
      perror("Failed to send data");
      close(udpSocket);
      return 1;
    }

    // std::cout << "Sent " << bytesSent << " bytes to the server" << std::endl;

    // Receive a response from the server
    char buffer[1024] = {0};
    socklen_t serverAddressLength = sizeof(serverAddress);
    ssize_t bytesReceived =
        recvfrom(udpSocket, buffer, sizeof(buffer), 0,
                 (struct sockaddr*)&serverAddress, &serverAddressLength);

    if (bytesReceived == -1) {
      perror("Error while receiving data");
    } else {
      buffer[bytesReceived] = '\0';
      if (FLAGS_verify) {
        msg_hdr_t* response;
        CHECK_EQ(bytesReceived, sizeof(msg_hdr_t));
        response = (msg_hdr_t*)buffer;
        CHECK_EQ(response->key, i);
        CHECK_EQ(response->value, i);
        std::cout << "key: " << response->key << " value: " << response->value << std::endl;
      }
      i++;
    }
  }

  // Close the socket
  close(udpSocket);

  return 0;
}
