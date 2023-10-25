#include <glog/logging.h>
#include <gflags/gflags.h>

#include <fcntl.h>

#include <chrono>
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

using std::chrono::duration_cast;
using std::chrono::high_resolution_clock;
using std::chrono::time_point;

DEFINE_uint64(num_keys, 1000, "Number of keys to insert and retrieve");
DEFINE_uint32(thread_size, 1, "Number of threads to use for the server application");
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

class ThreadCtx {
 private:

 public:
  ThreadCtx(const void *channel_ctx, const MachnetFlow_t *flow_info, const uint32_t listen_port)
      : channel_ctx(CHECK_NOTNULL(channel_ctx)), flow(flow_info), listen_port(listen_port){
    rx_message.resize(sizeof(msg_hdr_t));
    tx_message.resize(sizeof(msg_hdr_t));
  }
  ~ThreadCtx() {}



 public:
  const void *channel_ctx;
  const MachnetFlow_t *flow;
  std::vector<uint8_t> rx_message;
  std::vector<uint8_t> tx_message;
  uint32_t listen_port;
};

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

static constexpr uint16_t kPort = 888;

void MachnetTransportServer(uint8_t thread_id) {
  // Initialize machnet and attach

  void* channel = machnet_attach();
  CHECK(channel != nullptr) << "machnet_attach() failed";
  if (channel == nullptr) {
    LOG(INFO) << "machnet_attch() failed";
    return;
  }
  ThreadCtx thread_ctx(channel, nullptr /* flow info */, kPort + thread_id);

  int ret = machnet_listen(channel, FLAGS_local.c_str(), thread_ctx.listen_port);
  if (ret != 0) {
    LOG(INFO) << "machnet_listen() failed";
    return;
  }

  // Handle client requests
  LOG(INFO) << "Waiting for client requests";
  store.StartSession();

  while (true) {
    if (g_keep_running == 0) {
      LOG(INFO) << "MsgGenLoop: Exiting.";
      break;
    }

    MachnetFlow rx_flow;
    const ssize_t ret =
        machnet_recv(channel, thread_ctx.rx_message.data(), thread_ctx.rx_message.size(), &rx_flow);
    if (ret == 0) {
    //   usleep(1);
      continue;
    }

    const msg_hdr_t* msg_hdr =
        reinterpret_cast<const msg_hdr_t*>(thread_ctx.rx_message.data());

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

    msg_hdr_t* tx_msg_hdr = reinterpret_cast<msg_hdr_t*>(thread_ctx.tx_message.data());
    tx_msg_hdr->window_slot = msg_hdr->window_slot;
    tx_msg_hdr->key = msg_hdr->key;

    if (result == Status::Ok) {
      tx_msg_hdr->value = context.output;
    } else {
      tx_msg_hdr->value = 0;
      // LOG(WARNING) << "Key not found";
    }

    ssize_t send_ret = machnet_send(channel, tx_flow, thread_ctx.tx_message.data(),
                                    sizeof(thread_ctx.tx_message.data()));
    if (send_ret == -1) LOG(ERROR) << "machnet_send() failed";
  }

  store.StopSession();
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
  // server_addr.sin_addr.s_addr = htonl(std::strtoul(FLAGS_local.c_str(), nullptr, 10));
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(kPort + 10000);

  LOG(INFO) << "Binding to " << FLAGS_local << ":" << kPort + 10000;

  int ret = bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr));
  if (ret < 0) {
    LOG(FATAL) << "bind() failed, error: " << strerror(errno);
  }

  // Handle client requests
  LOG(INFO) << "Waiting for client requests";
  while (true) {

    if (g_keep_running == 0) {
      LOG(INFO) << "MsgGenLoop: Exiting.";
      break;
    }


    std::array<char, 1024> buf;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);


    const ssize_t ret =
        recvfrom(sockfd, buf.data(), sizeof(msg_hdr_t), 0,
                 (struct sockaddr *)&client_addr, &client_addr_len);

    const msg_hdr_t* msg_hdr =
        reinterpret_cast<const msg_hdr_t*>(buf.data());

    if (ret < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        usleep(1);
        continue;
      }
      LOG(FATAL) << "recvfrom() failed, error: " << strerror(errno);
    }

    VLOG(1) << "Received GET request for key: " << msg_hdr->key;

    auto callback = [](IAsyncContext* ctxt, Status result) {
      // In-memory test.
      CHECK_EQ(true, false);
    };

    uint64_t key = msg_hdr->key;

    ReadContext context{msg_hdr->key};
    Status result = store.Read(context, callback, 1);

    if (result == Status::Ok) {
      memset(buf.data(), 0, sizeof(buf));

      msg_hdr_t* tx_msg_hdr = reinterpret_cast<msg_hdr_t*>(buf.data());
      tx_msg_hdr->window_slot = msg_hdr->window_slot;
      tx_msg_hdr->key = key;
      tx_msg_hdr->value = context.output;

      ssize_t send_ret =
          sendto(sockfd, buf.data(), sizeof(struct msg_hdr_t), 0,
                 (struct sockaddr *)&client_addr, client_addr_len);
      VLOG(1) << "Sent value of size " << sizeof(context.output) << " bytes to client";
      if (send_ret == -1) {
        LOG(ERROR) << "sendto() failed";
      }
    } else {
      LOG(ERROR) << "Error retrieving key: " << key;
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
}



int main(int argc, char* argv[]) {
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  FLAGS_logtostderr = 1;

  signal(SIGINT, SigIntHandler);

  tx_message.resize(sizeof(msg_hdr_t));
  rx_message.resize(sizeof(msg_hdr_t));

  std::thread datapath_threads[FLAGS_thread_size];

  // Initializing machnet


  if (FLAGS_transport == "machnet") {
    int ret = machnet_init();
    CHECK_EQ(ret, 0) << "machnet_init() failed";
    Populate();
    for(size_t i = 0; i < FLAGS_thread_size; i++)
      datapath_threads[i] = std::thread(&MachnetTransportServer, i);
  } else if (FLAGS_transport == "udp") {
    Populate();
    datapath_threads[0] = std::thread(&UDPTransportServer);
    // UDPTransportServer();
  } else if (FLAGS_transport == "test") {
    datapath_threads[0] = std::thread(&run_key_value);
    // run_key_value();
  } else {
    LOG(FATAL) << "Unknown transport: " << FLAGS_transport;
  }

  while (g_keep_running) sleep(5);

  for(size_t i = 0; i < FLAGS_thread_size; i++)
    datapath_threads[i].join();

  store.StopSession();
  return 0;
}
