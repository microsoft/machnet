#include <fcntl.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <machnet.h>
#include <netinet/in.h>
#include <rocksdb/db.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <random>
#include <string>
#include <vector>

DEFINE_int32(num_keys, 1000, "Number of keys to insert and retrieve");
DEFINE_int32(num_probes, 10000, "Number of probes to perform");
DEFINE_int32(value_size, 200, "Size of value in bytes");
DEFINE_string(local, "", "Local IP address, needed for Machnet only");
DEFINE_string(transport, "machnet", "Transport to use (machnet, udp)");

static constexpr uint16_t kPort = 888;
const char kNsaasRocksDbServerFile[] = "/tmp/testdb";

void MachnetTransportServer(rocksdb::DB *db) {
  // Initialize machnet and attach
  int ret = machnet_init();
  CHECK_EQ(ret, 0) << "machnet_init() failed";
  void *channel = machnet_attach();

  CHECK(channel != nullptr) << "machnet_attach() failed";
  ret = machnet_listen(channel, FLAGS_local.c_str(), kPort);
  CHECK_EQ(ret, 0) << "machnet_listen() failed";

  // Handle client requests
  LOG(INFO) << "Waiting for client requests";
  while (true) {
    std::array<char, 1024> buf;
    MachnetFlow rx_flow;
    const ssize_t ret = machnet_recv(channel, buf.data(), buf.size(), &rx_flow);
    CHECK_GE(ret, 0) << "machnet_recv() failed";
    if (ret == 0) {
      usleep(1);
      continue;
    }

    std::string key(buf.data(), ret);
    VLOG(1) << "Received GET request for key: " << key;

    std::string value;
    const rocksdb::Status status = db->Get(rocksdb::ReadOptions(), key, &value);

    if (status.ok()) {
      MachnetFlow tx_flow;
      tx_flow.dst_ip = rx_flow.src_ip;
      tx_flow.src_ip = rx_flow.dst_ip;
      tx_flow.dst_port = rx_flow.src_port;
      tx_flow.src_port = rx_flow.dst_port;

      ssize_t send_ret =
          machnet_send(channel, tx_flow, value.data(), value.size());
      VLOG(1) << "Sent value of size " << value.size() << " bytes to client";
      if (send_ret == -1) {
        LOG(ERROR) << "machnet_send() failed";
      }
    } else {
      LOG(ERROR) << "Error retrieving key: " << status.ToString();
    }
  }
}

void UDPTransportServer(rocksdb::DB *db) {
  // Create and configure the UDP socket
  int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  CHECK_GE(sockfd, 0) << "socket() failed";
  int flags = fcntl(sockfd, F_GETFL, 0);
  CHECK_GE(flags, 0) << "fcntl() F_GETFL failed";
  CHECK_GE(fcntl(sockfd, F_SETFL, flags | O_NONBLOCK), 0)
      << "fcntl() F_SETFL failed";

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(kPort);

  int ret = bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr));
  if (ret < 0) {
    LOG(FATAL) << "bind() failed, error: " << strerror(errno);
  }

  // Handle client requests
  LOG(INFO) << "Waiting for client requests";
  while (true) {
    std::array<char, 1024> buf;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    const ssize_t ret =
        recvfrom(sockfd, buf.data(), buf.size(), 0,
                 (struct sockaddr *)&client_addr, &client_addr_len);
    if (ret < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        usleep(1);
        continue;
      }
      LOG(FATAL) << "recvfrom() failed, error: " << strerror(errno);
    }

    std::string key(buf.data(), ret);
    VLOG(1) << "Received GET request for key: " << key;

    std::string value;
    const rocksdb::Status status = db->Get(rocksdb::ReadOptions(), key, &value);

    if (status.ok()) {
      ssize_t send_ret =
          sendto(sockfd, value.data(), value.size(), 0,
                 (struct sockaddr *)&client_addr, client_addr_len);
      VLOG(1) << "Sent value of size " << value.size() << " bytes to client";
      if (send_ret == -1) {
        LOG(ERROR) << "sendto() failed";
      }
    } else {
      LOG(ERROR) << "Error retrieving key: " << status.ToString();
    }
  }
}

int main(int argc, char *argv[]) {
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  FLAGS_logtostderr = 1;

  // Initialize RocksDB
  rocksdb::DB *db;
  rocksdb::Options options;
  options.create_if_missing = true;
  LOG(INFO) << "Opening RocksDB, file = " << kNsaasRocksDbServerFile;
  rocksdb::Status status =
      rocksdb::DB::Open(options, kNsaasRocksDbServerFile, &db);
  if (!status.ok()) {
    LOG(ERROR) << "Error opening RocksDB: " << status.ToString();
    return 1;
  }

  // Insert num_keys string keys
  LOG(INFO) << "Inserting " << FLAGS_num_keys << " key-value pairs";
  for (int i = 0; i < FLAGS_num_keys; i++) {
    std::string key = "key" + std::to_string(i);
    std::string value = "value" + std::string(FLAGS_value_size, 'x');
    status = db->Put(rocksdb::WriteOptions(), key, value);
    if (!status.ok()) {
      LOG(ERROR) << "Error inserting key-value pair: " << status.ToString();
      return 1;
    }
  }

  if (FLAGS_transport == "machnet") {
    MachnetTransportServer(db);
  } else if (FLAGS_transport == "udp") {
    UDPTransportServer(db);
  } else {
    LOG(FATAL) << "Unknown transport: " << FLAGS_transport;
  }

  return 0;
}
