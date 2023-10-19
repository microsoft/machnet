#include <glog/logging.h>
#include <gflags/gflags.h>

#include <fcntl.h>

#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <thread>
#include <utility>

#include <machnet.h>
#include <netinet/in.h>

#include "core/faster.h"
#include "device/null_disk.h"
#include "test_types.h"

DEFINE_uint64(num_keys, 1000, "Number of keys to insert and retrieve");
DEFINE_int32(num_probes, 10000, "Number of probes to perform");
DEFINE_int32(value_size, 200, "Size of value in bytes");
DEFINE_string(local, "", "Local IP address, needed for Machnet only");
DEFINE_string(transport, "machnet", "Transport to use (machnet, udp)");


using namespace FASTER::core;
using FASTER::test::FixedSizeKey;
using FASTER::test::NonCopyable;
using FASTER::test::NonMovable;
using FASTER::test::SimpleAtomicValue;

struct msg_hdr_t {
  uint64_t window_slot;
  uint64_t key;
  uint64_t value;
};

std::vector<uint8_t> tx_message;
std::vector<uint8_t> rx_message;

static volatile int g_keep_running = 1;


void SigIntHandler([[maybe_unused]] int signal) { g_keep_running = 0; }


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

FasterKv<Key, Value, FASTER::device::NullDisk> store{1<<16, 1073741824, ""};

static constexpr uint16_t kPort = 888;

void MachnetTransportServer() {
  // Initialize machnet and attach

  int ret = machnet_init();
  CHECK_EQ(ret, 0) << "machnet_init() failed";
  void* channel = machnet_attach();

  CHECK(channel != nullptr) << "machnet_attach() failed";
  ret = machnet_listen(channel, FLAGS_local.c_str(), kPort);
  CHECK_EQ(ret, 0) << "machnet_listen() failed";

  // Handle client requests
  LOG(INFO) << "Waiting for client requests";
  while (true) {
    if (g_keep_running == 0) {
      LOG(INFO) << "MsgGenLoop: Exiting.";
      break;
    }

    MachnetFlow rx_flow;
    const ssize_t ret =
        machnet_recv(channel, rx_message.data(), rx_message.size(), &rx_flow);
    if (ret == 0) {
    //   usleep(1);
      continue;
    }

    const msg_hdr_t* msg_hdr =
        reinterpret_cast<const msg_hdr_t*>(rx_message.data());

    auto callback = [](IAsyncContext* ctxt, Status result) {
      // It is an in-memory store so, we never get here!
      CHECK_EQ(true, false);
    };

    ReadContext context{msg_hdr->key};
    Status result = store.Read(context, callback, 1);

    MachnetFlow tx_flow;
    tx_flow.dst_ip = rx_flow.src_ip;
    tx_flow.src_ip = rx_flow.dst_ip;
    tx_flow.dst_port = rx_flow.src_port;
    tx_flow.src_port = rx_flow.dst_port;

    msg_hdr_t* tx_msg_hdr = reinterpret_cast<msg_hdr_t*>(tx_message.data());
    tx_msg_hdr->window_slot = msg_hdr->window_slot;
    tx_msg_hdr->key = msg_hdr->key;

    if (result == Status::Ok) {
      tx_msg_hdr->value = context.output;
    } else {
      tx_msg_hdr->value = 0;
      LOG(WARNING) << "Key not found";
    }

    ssize_t send_ret = machnet_send(channel, tx_flow, tx_message.data(),
                                    sizeof(tx_message.data()));
    if (send_ret == -1) LOG(ERROR) << "machnet_send() failed";
  }
}

void UDPTransportServer() {
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

    auto callback = [](IAsyncContext* ctxt, Status result) {
      // In-memory test.
      CHECK_EQ(true, false);
    };

    ReadContext context{std::stoull(key)};
    Status result = store.Read(context, callback, 1);
    CHECK_EQ(Status::Ok, result);
    CHECK_EQ(23, context.output);

    if (result == Status::Ok) {
      ssize_t send_ret =
          sendto(sockfd, &context.output, sizeof(context.output), 0,
                 (struct sockaddr *)&client_addr, client_addr_len);
      VLOG(1) << "Sent value of size " << sizeof(context.output) << " bytes to client";
      if (send_ret == -1) {
        LOG(ERROR) << "sendto() failed";
      }
    } else {
      LOG(ERROR) << "Error retrieving key: " << "status.ToString()";
    }
  }
}

void run_key_value() {

  store.StartSession();

  for (size_t idx = 0; idx < 128; ++idx) {
    auto callback = [](IAsyncContext* ctxt, Status result) {
      // In-memory test.
      CHECK_EQ(true, false);
    };

    UpsertContext context{static_cast<uint64_t>(idx), static_cast<uint64_t>(idx)};
    Status result = store.Upsert(context, callback, 1);
    CHECK_EQ(Status::Ok, result);
  }

  // Read.
  for (size_t idx = 0; idx < 128; ++idx) {
    auto callback = [](IAsyncContext* ctxt, Status result) {
      // In-memory test.
      CHECK_EQ(true, false);
    };

    ReadContext context{static_cast<uint64_t>(idx)};
    Status result = store.Read(context, callback, 1);
    CHECK_EQ(Status::Ok, result);
    CHECK_EQ(idx, context.output);

    std::cout << "Read " << static_cast<uint64_t>(context.output) <<
    std::endl;
  }
  store.StopSession();
}

void Populate() {
  auto callback = [](IAsyncContext* ctxt, Status result) {
    CHECK_EQ(true, false);
  };

  for (size_t idx = 0; idx < FLAGS_num_keys; ++idx) {
    UpsertContext context{static_cast<uint64_t>(idx), static_cast<uint64_t>(idx)};
    Status result = store.Upsert(context, callback, 1);
    CHECK_EQ(Status::Ok, result);
  }
}



int main(int argc, char* argv[]) {
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  FLAGS_logtostderr = 1;

  signal(SIGINT, SigIntHandler);

  tx_message.resize(sizeof(msg_hdr_t));
  rx_message.resize(sizeof(msg_hdr_t));

  if (FLAGS_transport == "machnet") {
    Populate();
    store.StartSession();
    MachnetTransportServer();
    store.StopSession();
  } else if (FLAGS_transport == "udp") {
    UDPTransportServer();
  } else if (FLAGS_transport == "test") {
    run_key_value();
  } else {
    LOG(FATAL) << "Unknown transport: " << FLAGS_transport;
  }

  return 0;
}
