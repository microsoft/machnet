/**
 * @file shm_test.cc
 *
 * Unit tests for juggler's Channels (POSIX shared memory) driver.
 */
#include <channel.h>
#include <channel_msgbuf.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <machnet.h>
#include <unistd.h>
#include <utils.h>

#include <atomic>
#include <random>
#include <thread>

constexpr const char *file_name(const char *path) {
  const char *file = path;
  while (*path) {
    if (*path++ == '/') {
      file = path;
    }
  }
  return file;
}

const char *fname = file_name(__FILE__);

// Enqueue one message from the application to Machnet.
bool app_msg_enqueue(MachnetChannelCtx_t *ctx, const std::vector<char> &msg) {
  MachnetIovec_t iovec;
  iovec.base = const_cast<char *>(msg.data());
  iovec.len = msg.size();
  MachnetMsgHdr_t msghdr;
  msghdr.msg_size = msg.size();
  msghdr.flags = 0;
  msghdr.msg_iov = &iovec;
  msghdr.msg_iovlen = 1;

  return machnet_sendmsg(ctx, &msghdr) == 0;
}

// Check if the message we received in Machnet matches the original one sent
// from the application.
bool check_msg(juggler::shm::ShmChannel *channel, juggler::shm::MsgBuf *msg,
               const std::vector<char> &expected) {
  uint32_t expected_msg_ofs = 0;
  while (expected_msg_ofs < expected.size()) {
    if (msg->length() > expected.size() - expected_msg_ofs) return false;
    const int rc = std::memcmp(
        msg->head_data(), expected.data() + expected_msg_ofs, msg->length());
    if (rc != 0) return false;
    expected_msg_ofs += msg->length();
    if (!msg->has_next()) break;
    msg = channel->GetMsgBuf(msg->next());
  }

  if (expected_msg_ofs != expected.size()) return false;

  return true;
}

/**
 * @brief This helper function accepts a message (as vector) and copies the
 * payload to a `MsgBufBatch'.
 *
 * @param  batch      The `MsgBufBatch' to which the message will be copied.
 * @param  msg        The message to be copied.
 * @return bool       True if the message was copied successfully.
 */
bool machnet_msg_prepare(juggler::shm::MsgBufBatch *batch, const uint8_t *msg,
                         const uint32_t msg_size) {
  uint32_t msg_ofs = 0;
  uint32_t msg_buf_index = 0;
  while (msg_ofs < msg_size) {
    if (msg_buf_index >= batch->GetSize()) break;
    if (msg_buf_index > 0) {
      auto *prev_buf = batch->bufs()[msg_buf_index - 1];
      prev_buf->set_next(batch->buf_indices()[msg_buf_index]);
    }
    auto *msg_buf = batch->bufs()[msg_buf_index];
    assert(msg_buf->length() == 0);  // Expect a fresh buffer.

    auto nbytes = std::min(msg_size - msg_ofs, msg_buf->tailroom());
    auto *payload = msg_buf->append(nbytes);
    CHECK_NOTNULL(payload);
    juggler::utils::Copy(payload, msg + msg_ofs, nbytes);

    msg_buf_index++;
    msg_ofs += nbytes;
  }

  if (msg_ofs != msg_size) {
    LOG(INFO) << "Failed to copy message: " << msg_ofs << " != " << msg_size;
    return false;
  }

  return true;
}

TEST(BasicChannelTest, ChannelCreateDestroy) {
  using ChannelManager = juggler::shm::ChannelManager<juggler::shm::ShmChannel>;
  const uint32_t kChannelRingSize = 1 << 11;  // 2048 slots for all rings.
  const uint32_t kBufferSize = 1 << 12;       // 4096 bytes for buffer.

  ChannelManager channel_mgr;

  for (size_t counter = 0; counter < ChannelManager::kMaxChannelNr; counter++) {
    std::string channel_name =
        std::string(fname) + "-" + std::to_string(counter);
    EXPECT_TRUE(channel_mgr.AddChannel(channel_name.c_str(), kChannelRingSize,
                                       kChannelRingSize, kChannelRingSize,
                                       kBufferSize));
  }

  EXPECT_EQ(channel_mgr.GetChannelCount(), ChannelManager::kMaxChannelNr);

  size_t counter = 0;
  for (counter = 0; counter < ChannelManager::kMaxChannelNr; counter++) {
    std::string channel_name =
        std::string(fname) + "-" + std::to_string(counter);
    EXPECT_NE(channel_mgr.GetChannel(channel_name.c_str()), nullptr);
  }
}

TEST(BasicChannelTest, ChannelMsgBufAllocFree) {
  using ChannelManager = juggler::shm::ChannelManager<juggler::shm::ShmChannel>;
  const uint32_t kChannelRingSize = 1 << 11;  // 2048 slots for all rings.
  const uint32_t kBufferSize = 1 << 12;       // 4096 bytes for buffer.

  ChannelManager channel_mgr;

  std::string channel_name(fname);
  EXPECT_TRUE(channel_mgr.AddChannel(channel_name.c_str(), kChannelRingSize,
                                     kChannelRingSize, kChannelRingSize,
                                     kBufferSize));
  auto *channel = channel_mgr.GetChannel(channel_name.c_str()).get();
  CHECK_NOTNULL(channel);
  auto *msg_buf = channel->MsgBufAlloc();
  EXPECT_NE(msg_buf, nullptr);
  EXPECT_TRUE(channel->MsgBufFree(msg_buf));
}

TEST(BasicChannelTest, ChannelDequeue) {
  using ChannelManager = juggler::shm::ChannelManager<juggler::shm::ShmChannel>;
  const uint32_t kChannelRingSize = 1 << 11;  // 2048 slots for all rings.
  const uint32_t kBufferSize = 1 << 12;       // 4096 bytes for buffer.
  const uint32_t kMessageSize = 1 << 15;      // 32K bytes for message.

  ChannelManager channel_mgr;

  std::string channel_name(fname);
  EXPECT_TRUE(channel_mgr.AddChannel(channel_name.c_str(), kChannelRingSize,
                                     kChannelRingSize, kChannelRingSize,
                                     kBufferSize));
  auto *channel = channel_mgr.GetChannel(channel_name.c_str()).get();
  CHECK_NOTNULL(channel);

  // Step 1: Enqueue some messages to the channel (from the application).
  const std::vector<char> msg1(kMessageSize, 'a');
  EXPECT_TRUE(
      app_msg_enqueue(const_cast<MachnetChannelCtx_t *>(channel->ctx()), msg1));
  const std::vector<char> msg2(kMessageSize, 'b');
  EXPECT_TRUE(
      app_msg_enqueue(const_cast<MachnetChannelCtx_t *>(channel->ctx()), msg2));

  // Step 2: Dequeue the messages from the application, and check validity.
  juggler::shm::MsgBufBatch batch;
  channel->DequeueMessages(&batch);
  EXPECT_EQ(batch.GetSize(), 2);
  EXPECT_TRUE(check_msg(channel, batch.bufs()[0], msg1));
  EXPECT_TRUE(check_msg(channel, batch.bufs()[1], msg2));
}

TEST(BasicChannelTest, ChannelEnqueue) {
  const uint32_t kChannelRingSize = 1 << 11;  // 2048 slots for all rings.
  const uint32_t kBufferSize = 1 << 12;       // 4096 bytes for buffer.
  const uint32_t kMessageSize = 1 << 15;      // 32K bytes for message.

  juggler::shm::ChannelManager channel_mgr;

  std::string channel_name(fname);
  EXPECT_TRUE(channel_mgr.AddChannel(channel_name.c_str(), kChannelRingSize,
                                     kChannelRingSize, kChannelRingSize,
                                     kBufferSize));
  auto *channel = channel_mgr.GetChannel(channel_name.c_str()).get();
  CHECK_NOTNULL(channel);

  // Step 1: Enqueue some messages to the channel (from the application).
  const std::vector<uint8_t> tx_msg(kMessageSize, 'a');
  auto buf_size = channel->GetUsableBufSize();
  auto nbuffers = (kMessageSize + buf_size - 1) / buf_size;

  // Allocate the MsgBufs we need to accommodate the message.
  juggler::shm::MsgBufBatch batch;
  EXPECT_TRUE(channel->MsgBufBulkAlloc(&batch, nbuffers));
  EXPECT_EQ(batch.GetSize(), nbuffers);
  EXPECT_TRUE(machnet_msg_prepare(&batch, tx_msg.data(), tx_msg.size()));
  EXPECT_EQ(channel->EnqueueMessages(&batch.bufs()[0], 1), 1);

  // Step 2: Dequeue the message from the application side, and check validity.
  std::vector<uint8_t> rx_msg(kMessageSize);
  MachnetIovec_t rx_iov;
  rx_iov.base = rx_msg.data();
  rx_iov.len = rx_msg.size();
  MachnetMsgHdr_t rx_msghdr;
  rx_msghdr.msg_size = 0;
  rx_msghdr.flow_info = {
      .src_ip = 0, .dst_ip = 0, .src_port = 0, .dst_port = 0};
  rx_msghdr.msg_iov = &rx_iov;
  rx_msghdr.msg_iovlen = 1;

  EXPECT_EQ(machnet_recvmsg(channel->ctx(), &rx_msghdr), 1);
  EXPECT_EQ(rx_msghdr.msg_size, kMessageSize);
  EXPECT_EQ(rx_msg, tx_msg);
}

TEST(ChannelFullDuplex, SendRecvMsg) {
  const std::chrono::milliseconds kTimeoutMs =
      std::chrono::milliseconds(60 * 1000);   // 60 seconds.
  const uint32_t kChannelRingSize = 1 << 10;  // 1024 slots for all rings.
  const uint32_t kBufferSize =
      (1 << 12) + MACHNET_MSGBUF_SPACE_RESERVED;  // 4096 bytes for payload.
  const std::size_t kMsgSizeMin = 4;
  const std::size_t kMsgSizeMax = 1 << 17;  // 128K bytes.
  const size_t kMsgNr = 1 << 20;

  std::vector<uint8_t> tx_msg(kMsgSizeMax, 0xff);
  std::vector<uint8_t> rx_msg(kMsgSizeMax);

  enum exit_code_t : int {
    kSuccess = 0,
    kBindFailed = 1,
    kTimeoutExpired = 2,
    kError = 3,
  };
  exit_code_t error = kSuccess;

  std::random_device dev;
  std::mt19937 rng(dev());
  std::uniform_int_distribution<std::mt19937::result_type> gen_msg_size(
      kMsgSizeMin, kMsgSizeMax);
  std::string channel_name(fname);
  juggler::shm::ChannelManager channel_mgr;

  // Create a channel.
  EXPECT_TRUE(channel_mgr.AddChannel(channel_name.c_str(), kChannelRingSize,
                                     kChannelRingSize, kChannelRingSize,
                                     kBufferSize));
  auto *channel = channel_mgr.GetChannel(channel_name.c_str()).get();
  CHECK_NOTNULL(channel);

  pid_t pid = fork();
  if (pid != 0) {
    // Parent process.
    // Busy-wait until the child process is ready.
    while (__machnet_channel_app_ring_pending(channel->ctx()) == 0) {
      __asm__("pause");
    }

    size_t msg_tx = 0, msg_rx = 0;
    auto start = std::chrono::system_clock::now();
    while (msg_tx < kMsgNr || msg_rx < kMsgNr) {
      // Check whether the timeout has expired.
      auto now = std::chrono::system_clock::now();
      auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
      if (elapsed.count() > kTimeoutMs.count()) {
        error = kTimeoutExpired;
        break;
      }

      // Dequeue messages from the channel.
      juggler::shm::MsgBufBatch rx_batch;
      channel->DequeueMessages(&rx_batch);
      for (uint16_t i = 0; i < rx_batch.GetSize(); i++) {
        msg_rx++;

        // Release all buffers of this message.
        auto *msg_buf = rx_batch.bufs()[i];
        auto msg_buf_index = rx_batch.buf_indices()[i];
        EXPECT_LT(msg_buf_index, channel->GetTotalBufCount());

        juggler::shm::MsgBufBatch cur_msg;
        cur_msg.Append(msg_buf, msg_buf_index);

        // Follow the chain of buffers.
        while (msg_buf->has_next()) {
          msg_buf_index = msg_buf->next();
          EXPECT_LT(msg_buf_index, channel->GetTotalBufCount());
          msg_buf = channel->GetMsgBuf(msg_buf_index);
          cur_msg.Append(msg_buf, msg_buf_index);
        }

        // Now release all buffers in the chain.
        EXPECT_TRUE(channel->MsgBufBulkFree(&cur_msg));
      }

      // Enqueue a message to the channel.
      if (msg_tx == kMsgNr) continue;
      size_t tx_msg_size = gen_msg_size(rng);
      juggler::shm::MsgBufBatch tx_batch;
      auto nbuffers = (tx_msg_size + channel->GetUsableBufSize() - 1) /
                      channel->GetUsableBufSize();
      EXPECT_LE(nbuffers, juggler::shm::MsgBufBatch::kMaxBurst);
      auto ret = channel->MsgBufBulkAlloc(&tx_batch, nbuffers);
      if (!ret) continue;  // No buffers available.

      EXPECT_TRUE(machnet_msg_prepare(&tx_batch, tx_msg.data(), tx_msg_size));
      ret = channel->EnqueueMessages(&tx_batch.bufs()[0], 1);
      if (ret == 1)
        msg_tx++;
      else
        // Release the buffers.
        EXPECT_TRUE(channel->MsgBufBulkFree(&tx_batch));
    }

    int wstatus;
    waitpid(pid, &wstatus, 0);

    // The child is going to check the pattern, and exit with code 0 on success.
    EXPECT_EQ(WEXITSTATUS(wstatus), kSuccess);
    EXPECT_EQ(msg_tx, kMsgNr);
    EXPECT_EQ(msg_rx, kMsgNr);
    EXPECT_EQ(error, kSuccess);

    // Check the buffer pool status.
    EXPECT_EQ(channel->GetFreeBufCount(), channel->GetTotalBufCount());

    std::vector<MachnetRingSlot_t> expected_buffers(
        channel->GetTotalBufCount());
    std::iota(expected_buffers.begin(), expected_buffers.end(), 0);
    std::vector<MachnetRingSlot_t> buffers;
    uint32_t buffers_size = channel->GetTotalBufCount();

    // Allocate all the buffers in the channel in 3 parts
    // Allocate from ctx->cache
    uint32_t current_buffers_cnt;
    auto ctx = channel->ctx();
    while (ctx->app_buffer_cache.count > 0)
      buffers.push_back(
          ctx->app_buffer_cache.indices[--ctx->app_buffer_cache.count]);
    current_buffers_cnt = buffers.size();
    // Allocate from shm::channel->cache
    current_buffers_cnt += channel->GetAllCachedBufferIndices(&buffers);
    // Allocate from ring
    buffers.resize(buffers_size);
    current_buffers_cnt += __machnet_channel_buf_alloc_bulk(
        channel->ctx(), buffers.size() - current_buffers_cnt,
        buffers.data() + current_buffers_cnt, nullptr);
    EXPECT_EQ(current_buffers_cnt, buffers.size());
    EXPECT_EQ(channel->GetFreeBufCount(), 0);
    sort(buffers.begin(), buffers.end());
    EXPECT_EQ(buffers, expected_buffers);

  } else {
    // Child process.
    const int kNumRetries = 5;
    size_t channel_size;
    MachnetChannelCtx_t *ctx = nullptr;

    // Attempt to bind to the channel. Channel might not be ready yet, so we
    // retry.
    int shm_fd = channel->GetFd();
    int num_retries = 0;
    while (ctx == nullptr) {
      if (num_retries++ > kNumRetries) {
        exit(kBindFailed);
      }
      ctx = machnet_bind(shm_fd, &channel_size);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    auto start = std::chrono::system_clock::now();
    size_t msg_tx = 0, msg_rx = 0;

    while (msg_rx < kMsgNr || msg_tx < kMsgNr) {
      // First check whether the timeout has elapsed.
      auto now = std::chrono::system_clock::now();
      auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
      if (elapsed.count() > kTimeoutMs.count()) {
        LOG(INFO) << "TX: " << msg_tx << " RX: " << msg_rx;
        error = kTimeoutExpired;
        break;
      }

      // Receive messages.
      MachnetIovec_t iov;
      iov.base = rx_msg.data();
      iov.len = rx_msg.size();
      MachnetMsgHdr_t msghdr;
      msghdr.msg_size = 0;
      msghdr.flags = 0;
      msghdr.flow_info = {
          .src_ip = 0, .dst_ip = 0, .src_port = 0, .dst_port = 0};
      auto ret = machnet_recvmsg(ctx, &msghdr);
      if (ret == 1) msg_rx++;

      // If already sent the amount of messages needed skip.
      if (msg_tx == kMsgNr) continue;

      // Send a message.
      size_t tx_msg_size = gen_msg_size(rng);
      iov.base = tx_msg.data();
      iov.len = tx_msg_size;
      msghdr.msg_size = tx_msg_size;
      msghdr.flow_info = {.src_ip = UINT32_MAX,
                          .dst_ip = UINT32_MAX,
                          .src_port = UINT16_MAX,
                          .dst_port = UINT16_MAX};
      msghdr.msg_iov = &iov;
      msghdr.msg_iovlen = 1;
      ret = machnet_sendmsg(ctx, &msghdr);
      if (ret == 0) msg_tx++;
    }

    exit(error);
  }
}

int main(int argc, char **argv) {
  ::google::InitGoogleLogging(argv[0]);
  testing::InitGoogleTest(&argc, argv);

  return RUN_ALL_TESTS();
}
