#include <channel.h>
#include <channel_msgbuf.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <machnet.h>
#include <machnet_common.h>
#include <signal.h>
#include <ttime.h>
#include <unistd.h>
#include <utils.h>

#include <numeric>
#include <thread>

static constexpr uint8_t kStackCpuCoreId = 3;
static constexpr uint8_t kAppCpuCoreId = 5;

using ShmChannel = juggler::shm::ShmChannel;
using ChannelManager = juggler::shm::ChannelManager<ShmChannel>;

constexpr const char *file_name(const char *path) {
  const char *file = path;
  while (*path) {
    if (*path++ == '/') {
      file = path;
    }
  }
  return file;
}

const char *channel_name = file_name(__FILE__);
const uint32_t kRingSlotEntries = 1 << 8;
const uint32_t kBuffersNr = 1 << 9;
const uint32_t kBufferSize = 1500;  // MTU
static std::atomic<bool> g_start{false};
static std::atomic<bool> g_should_stop{false};

struct thread_conf {
  thread_conf(std::shared_ptr<ShmChannel> ch, uint8_t core_id,
              uint64_t messages_to_send, uint64_t tx_message_size,
              uint64_t messages_to_receive)
      : channel(ch),
        cpu_core(core_id),
        messages_to_send(messages_to_send),
        tx_message_size(tx_message_size),
        messages_to_receive(messages_to_receive),
        messages_sent(0),
        messages_received(0),
        duration_in_ns(0),
        finished(false) {}
  std::shared_ptr<ShmChannel> channel;
  uint8_t cpu_core;
  uint64_t messages_to_send;
  uint64_t tx_message_size;
  uint64_t messages_to_receive;
  uint64_t messages_sent;
  uint64_t messages_received;
  uint64_t duration_in_ns;
  bool finished{false};
};

void stack_loop(thread_conf *conf) {
  juggler::utils::BindThisThreadToCore(conf->cpu_core);
  LOG(INFO) << "Starting the stack thread on core " << sched_getcpu();
  const uint32_t kDummyIp = 0x0100007f;
  const uint16_t kDummyPort = 1234;
  auto &channel = conf->channel;
  CHECK_NOTNULL(channel);
  const auto buffers_per_msg =
      conf->tx_message_size / channel->GetUsableBufSize();
  std::vector<MachnetRingSlot_t> buffer_indexes;
  buffer_indexes.reserve(buffers_per_msg);
  std::vector<uint8_t> tx_buffer(conf->tx_message_size);
  std::iota(tx_buffer.begin(), tx_buffer.end(), 0);

  // Wait for flag to start.
  while (!g_start.load()) {
    __asm__ volatile("pause" ::: "memory");
  }

  // Use a high precision timer to measure time in nanoseconds for this
  // experiment (std::chrono) Start the timer.
  auto start = std::chrono::high_resolution_clock::now();
  while (!g_should_stop.load()) {
    // Receive any messages.
    juggler::shm::MsgBuf *buf;
    MachnetRingSlot_t slot;

    auto ret = channel->DequeueMessages(&slot, &buf, 1);
    if (ret == 1) {
      // We have received one message.
      conf->messages_received++;
      buffer_indexes.emplace_back(slot);
      while (buf->has_next()) {
        buffer_indexes.emplace_back(buf->next());
        buf = channel->GetMsgBuf(buf->next());
      }

      CHECK(channel->MsgBufBulkFree(buffer_indexes.data(),
                                    buffer_indexes.size()));
      buffer_indexes.clear();
    }

    if (conf->messages_sent >= conf->messages_to_send) {
      if (conf->messages_received >= conf->messages_to_receive) {
        break;
      }
      continue;
    }

    buf = channel->MsgBufAlloc();
    if (buf == nullptr) {
      continue;
    }

    CHECK_NOTNULL(buf->append(tx_buffer.size()));

    juggler::utils::Copy(buf->head_data(), tx_buffer.data(), buf->length());
    buf->set_src_ip(kDummyIp);
    buf->set_src_port(kDummyPort);
    buf->set_dst_ip(kDummyIp);
    buf->set_dst_port(kDummyPort);
    buf->mark_first();
    buf->mark_last();
    // TODO(ilias): Copy some data.
    // Send the message.
    ret = channel->EnqueueMessages(&buf, 1);
    if (ret != 1) {
      LOG(ERROR) << "Couldn't enqueue message. ret: " << ret;
      channel->MsgBufFree(buf);
    }
    conf->messages_sent += ret;
  }
  // Calculate the duration in nanoseconds.
  auto end = std::chrono::high_resolution_clock::now();
  conf->duration_in_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

  if (conf->messages_sent < conf->messages_to_send ||
      conf->messages_received < conf->messages_to_receive) {
    LOG(ERROR) << "Not all messages were sent. Sent: " << conf->messages_sent
               << ", received: " << conf->messages_received
               << " expected: " << conf->messages_to_send << ", "
               << conf->messages_to_receive << ". Stack thread exiting.";
  }

  conf->finished = true;
}

void application_loop(thread_conf *conf) {
  juggler::utils::BindThisThreadToCore(conf->cpu_core);
  LOG(INFO) << "Starting the app thread on core " << sched_getcpu();
  std::vector<uint8_t> rx_buffer(conf->tx_message_size);
  std::vector<uint8_t> tx_buffer(conf->tx_message_size);
  std::iota(tx_buffer.begin(), tx_buffer.end(), 0);
  auto &channel = conf->channel;

  // Wait for flag to start.
  while (!g_start.load()) {
    __asm__ volatile("pause" ::: "memory");
  }

  // Now start receiving messages.
  auto start = std::chrono::high_resolution_clock::now();
  while (!g_should_stop.load()) {
    // RX.
    MachnetFlow_t flow;

    auto nbytes =
        machnet_recv(channel->ctx(), rx_buffer.data(), rx_buffer.size(), &flow);
    if (nbytes > 0) {
      conf->messages_received++;
      CHECK_EQ(nbytes, conf->tx_message_size);
    }

    if (conf->messages_sent >= conf->messages_to_send) {
      if (conf->messages_received >= conf->messages_to_receive) {
        break;
      }
      continue;
    }

    // TX.
    auto ret =
        machnet_send(channel->ctx(), flow, tx_buffer.data(), tx_buffer.size());
    if (ret == 0) {
      conf->messages_sent++;
    }
  }

  // Calculate the duration in nanoseconds.
  auto end = std::chrono::high_resolution_clock::now();
  conf->duration_in_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

  if (conf->messages_sent < conf->messages_to_send ||
      conf->messages_received < conf->messages_to_receive) {
    LOG(ERROR) << "Not all messages were sent. Sent: " << conf->messages_sent
               << ", received: " << conf->messages_received
               << " expected: " << conf->messages_to_send << ", "
               << conf->messages_to_receive << ". App thread exiting.";
  }

  conf->finished = true;
}

void print_results(const thread_conf &stack_conf, const thread_conf &app_conf) {
  // Print the results.
  auto channel = stack_conf.channel;
  std::cout << juggler::utils::Format(
                   "[Channel:%s/HugePageBacked:%s/Size:%luKiB/BufferNr:%u/"
                   "BufSize:%u]",
                   channel_name, channel->IsPosixShm() ? "0" : "1",
                   channel->GetSize() / 1024, channel->GetTotalBufCount(),
                   channel->GetUsableBufSize())
            << std::endl;
  // Print whether the stack and app are sending or not.
  std::cout << juggler::utils::Format("[StackTX:%s/AppTX:%s]",
                                      stack_conf.messages_to_send ? "1" : "0",
                                      app_conf.messages_to_send ? "1" : "0");
  std::cout << juggler::utils::Format("[TX message size:%lu]",
                                      stack_conf.tx_message_size)
            << std::endl;
  std::cout << "[Stack]" << std::endl;

  std::cout
      << juggler::utils::Format(
             "\t[TX] %lu messages, %lu bytes, %lu ns, %lf ns/msg, %lf msg/s",
             stack_conf.messages_sent,
             stack_conf.messages_sent * stack_conf.tx_message_size,
             stack_conf.duration_in_ns,
             stack_conf.messages_sent
                 ? static_cast<double>(stack_conf.duration_in_ns) /
                       stack_conf.messages_sent
                 : 0.0,
             static_cast<double>(stack_conf.messages_sent) /
                 (static_cast<double>(stack_conf.duration_in_ns) / 1e9))
      << std::endl;
  std::cout
      << juggler::utils::Format(
             "\t[RX] %lu messages, %lu bytes, %lu ns, %lf ns/msg, %lf msg/s",
             stack_conf.messages_received,
             stack_conf.messages_received * stack_conf.tx_message_size,
             stack_conf.duration_in_ns,
             stack_conf.messages_received
                 ? static_cast<double>(stack_conf.duration_in_ns) /
                       stack_conf.messages_received
                 : 0.0,
             stack_conf.messages_received
                 ? static_cast<double>(stack_conf.messages_received) /
                       (static_cast<double>(stack_conf.duration_in_ns) / 1e9)
                 : 0.0)
      << std::endl;
  std::cout << "[Application]" << std::endl;
  std::cout << juggler::utils::Format(
                   "\t[TX]"
                   " %lu messages, %lu bytes, %lu ns, %lf ns/msg, %lf msg/s",
                   app_conf.messages_sent,
                   app_conf.messages_sent * app_conf.tx_message_size,
                   app_conf.duration_in_ns,
                   app_conf.messages_sent
                       ? static_cast<double>(app_conf.duration_in_ns) /
                             static_cast<double>(app_conf.messages_sent)
                       : 0.0,
                   app_conf.messages_sent
                       ? static_cast<double>(app_conf.messages_sent) /
                             (static_cast<double>(app_conf.duration_in_ns /
                                                  1e9))
                       : 0.0)
            << std::endl;
  std::cout << juggler::utils::Format(
                   "\t[RX]"
                   " %lu messages, %lu bytes, %lu ns, %lf ns/msg, %lf msg/s",
                   app_conf.messages_received,
                   app_conf.messages_received * app_conf.tx_message_size,
                   app_conf.duration_in_ns,
                   app_conf.messages_received
                       ? static_cast<double>(app_conf.duration_in_ns) /
                             static_cast<double>(app_conf.messages_received)
                       : 0.0,
                   app_conf.messages_received
                       ? static_cast<double>(app_conf.messages_received) /
                             (static_cast<double>(app_conf.duration_in_ns /
                                                  1e9))
                       : 0.0)
            << std::endl;
  std::cout << std::endl;
}

int main() {
  google::InitGoogleLogging("channel_bench");
  FLAGS_logtostderr = 1;
  signal(SIGINT, [](int) { g_should_stop.store(true); });

  if (geteuid() != 0) {
    LOG(ERROR) << "Must be run as root.";
    return -1;
  }
  LOG(INFO) << "Creating channel " << channel_name;
  ChannelManager channel_manager;
  CHECK(channel_manager.AddChannel(channel_name, kRingSlotEntries,
                                   kRingSlotEntries, kBuffersNr, kBufferSize));

  const uint64_t kMessagesToSend = 2 * 1e7;
  const uint64_t kTxMessageSize = 64;
  std::vector<std::pair<uint64_t, uint64_t>> exp_config_vec;

  exp_config_vec.emplace_back(kMessagesToSend, 0);  // Stack -> app only
  exp_config_vec.emplace_back(0, kMessagesToSend);  // App -> stack only
  exp_config_vec.emplace_back(kMessagesToSend, kMessagesToSend);  // Bi-dir

  LOG(INFO) << "Running channel_bench";

  for (const auto &exp_conf : exp_config_vec) {
    LOG(INFO) << "Running experiment: Stack will send " << exp_conf.first
              << " messages, App will send " << exp_conf.second << " messages.";

    thread_conf stack_conf{channel_manager.GetChannel(channel_name),
                           kStackCpuCoreId, exp_conf.first, kTxMessageSize,
                           exp_conf.second};
    thread_conf app_conf{channel_manager.GetChannel(channel_name),
                         kAppCpuCoreId, exp_conf.second, kTxMessageSize,
                         exp_conf.first};

    // Launch the threads.
    std::thread(&stack_loop, &stack_conf).detach();
    std::thread(&application_loop, &app_conf).detach();
    usleep(500000);
    g_start.store(true);

    const uint32_t kTimeoutSeconds = 30;
    uint32_t seconds_passed = 0;
    while (seconds_passed < kTimeoutSeconds) {
      if (stack_conf.finished && app_conf.finished) {
        break;
      } else {
        LOG(INFO) << "Main: Waiting for threads to finish. "
                  << "Stack finished: " << stack_conf.finished
                  << ", App finished: " << app_conf.finished;
      }
      seconds_passed++;
      sleep(1);
    }

    if (!stack_conf.finished || !app_conf.finished) {
      g_should_stop.store(true);
      LOG(INFO) << "Timeout reached. Stopping the threads.";
      sleep(1);
    }

    print_results(stack_conf, app_conf);
  }

  return 0;
}
