#include "machnet_private.h"

#include <glog/logging.h>
#include <gtest/gtest.h>
#include <utils.h>

#include <random>
#include <thread>

#include "pause.h"

TEST(MachnetPrivateTest, NSaasChannelCalcShMemSize) {
  auto calc_func = [](size_t machnet_r_slots, size_t app_r_slots,
                      size_t buf_r_slots, size_t buffer_size) {
    return __machnet_channel_dataplane_calculate_size(
        machnet_r_slots, app_r_slots, buf_r_slots, buffer_size, 0);
  };

  const uint32_t kMaxCount = std::min(65536u, RING_SZ_MASK);
  uint32_t count = 0;

  // Check return values for different ring slot numbers.
  // Rings should have sizes that are a power of 2.
  do {
    const uint32_t kBufferSize = 4096;
    auto result = calc_func(count, count, count, kBufferSize);
    if (juggler::utils::is_power_of_two(count))
      EXPECT_NE(result, std::size_t(-1));
    else
      EXPECT_EQ(result, std::size_t(-1));
    count++;
  } while (count <= kMaxCount);
}

TEST(MachnetPrivateTest, NSaasChannelCreateDestroy) {
  const std::size_t kChannelRingSize = 1 << 11;  // 2048 slots for all rings.
  const std::size_t kBufferSize = 1 << 12;       // 4096 bytes for buffer.
  const std::string channel_name = "test_channel_create_destroy";

  // Create a new channel (should succeed).
  size_t channel_size;
  int is_posix_shm;
  int channel_fd;
  MachnetChannelCtx_t *channel_ctx = __machnet_channel_create(
      channel_name.c_str(), kChannelRingSize, kChannelRingSize,
      kChannelRingSize, kBufferSize, &channel_size, &is_posix_shm, &channel_fd);
  EXPECT_NE(channel_ctx, nullptr);

  // Destroy the channel (should succeed).
  __machnet_channel_destroy(channel_ctx, channel_size, &channel_fd,
                            is_posix_shm, channel_name.c_str());
  EXPECT_EQ(channel_fd, -1);
}

TEST(MachnetPrivateTest, NSaasChannelDataplaneInit) {
  const uint32_t kChannelRingSize = 1 << 11;  // 2048 slots for all rings.
  const uint32_t kBufferSize = 1 << 12;       // 4096 bytes for buffer.
  const std::string channel_name = "test_channel";

  // Create a new channel (should succeed).
  size_t channel_size;
  int is_posix_shm;
  int channel_fd;
  MachnetChannelCtx_t *channel_ctx = __machnet_channel_create(
      channel_name.c_str(), kChannelRingSize, kChannelRingSize,
      kChannelRingSize, kBufferSize, &channel_size, &is_posix_shm, &channel_fd);
  EXPECT_NE(channel_ctx, nullptr);
  EXPECT_EQ(channel_ctx->magic, MACHNET_CHANNEL_CTX_MAGIC);
  EXPECT_EQ(std::string(channel_ctx->name), channel_name);

  // Destroy the channel (should succeed).
  __machnet_channel_destroy(channel_ctx, channel_size, &channel_fd,
                            is_posix_shm, channel_name.c_str());
  EXPECT_EQ(channel_fd, -1);
}

TEST(MachnetPrivateTest, NSaasChannelBufAllocFree) {
  const uint32_t kChannelRingSize = 1 << 11;  // 2048 slots for all rings.
  const uint32_t kBufferSize = 1 << 12;       // 4096 bytes for buffer.

  const std::string channel_name = "test_channel";

  // Create a new channel (should succeed).
  size_t channel_size;
  int is_posix_shm;
  int channel_fd;
  auto *channel = __machnet_channel_create(
      channel_name.c_str(), kChannelRingSize, kChannelRingSize,
      kChannelRingSize, kBufferSize, &channel_size, &is_posix_shm, &channel_fd);
  EXPECT_NE(channel, nullptr);
  EXPECT_EQ(channel->magic, MACHNET_CHANNEL_CTX_MAGIC);
  EXPECT_EQ(std::string(channel->name), channel_name);

  std::vector<MachnetRingSlot_t> buffer_indices;
  // TEST 0: Allocate a single buffer and free it (should succeed).
  buffer_indices.resize(1);
  uint32_t n = __machnet_channel_buf_alloc_bulk(channel, buffer_indices.size(),
                                                buffer_indices.data(), nullptr);
  EXPECT_EQ(n, buffer_indices.size());
  EXPECT_EQ(buffer_indices[0], 0);
  n = __machnet_channel_buf_free_bulk(channel, buffer_indices.size(),
                                      buffer_indices.data());
  EXPECT_EQ(n, buffer_indices.size());

  // TEST 1: Allocate all buffers, shuffle them, release them and check order.
  // NOTE: The usable buffers in the channel are equal to the buffer ring size
  // - 1.
  buffer_indices.resize(kChannelRingSize - 1);
  n = __machnet_channel_buf_alloc_bulk(channel, buffer_indices.size(),
                                       buffer_indices.data(), nullptr);
  EXPECT_EQ(n, buffer_indices.size());

  // Shuffle buffer_indices, and release them.
  std::random_shuffle(buffer_indices.begin(), buffer_indices.end());
  n = __machnet_channel_buf_free_bulk(channel, buffer_indices.size(),
                                      buffer_indices.data());
  EXPECT_EQ(n, buffer_indices.size());

  // Check order at allocation.
  auto original_buffer_indices = buffer_indices;
  std::vector<MachnetMsgBuf_t *> buffers;
  n = __machnet_channel_buf_alloc_bulk(channel, buffer_indices.size(),
                                       buffer_indices.data(), buffers.data());
  EXPECT_EQ(n, buffer_indices.size());
  EXPECT_EQ(original_buffer_indices, buffer_indices);

  // Check that all the Msg Buffers are initialized.
  for (auto &msg_buf : buffers) {
    EXPECT_EQ(msg_buf->magic, MACHNET_MSGBUF_MAGIC);
    EXPECT_EQ(msg_buf->size, kBufferSize);
    EXPECT_EQ(msg_buf->data_len, 0);
    EXPECT_EQ(msg_buf->flags, 0);
    EXPECT_EQ(msg_buf->next, UINT32_MAX);
  }

  // Destroy the channel (should succeed).
  __machnet_channel_destroy(channel, channel_size, &channel_fd, is_posix_shm,
                            channel_name.c_str());
  EXPECT_EQ(channel_fd, -1);
}

TEST(MachnetBufferPool, Concurrency) {
  const uint32_t kChannelRingSize = 1 << 8;  // 256 slots for all rings.
  const uint32_t kBufferSize = 1 << 8;       // 256 bytes for buffer.

  const std::string channel_name = "test_channel";

  // Create a new channel (should succeed).
  size_t channel_size;
  int is_posix_shm;
  int channel_fd;
  auto *channel = __machnet_channel_create(
      channel_name.c_str(), kChannelRingSize, kChannelRingSize,
      kChannelRingSize, kBufferSize, &channel_size, &is_posix_shm, &channel_fd);
  EXPECT_NE(channel, nullptr);
  EXPECT_EQ(channel->magic, MACHNET_CHANNEL_CTX_MAGIC);
  EXPECT_EQ(std::string(channel->name), channel_name);

  // Set the min and maximum buffers to allocate.
  const uint32_t kMinAllocNr = 1;
  const uint32_t kMaxAllocNr = 16;
  std::vector<uint8_t> allocs_nr(1e7);

  std::mt19937 engine;
  std::uniform_int_distribution<uint8_t> dist(kMinAllocNr, kMaxAllocNr);
  for (auto &alloc_nr : allocs_nr) {
    alloc_nr = dist(engine);
  }

  const size_t kThreadsNr = 4;
  std::atomic<uint32_t> thread_count(kThreadsNr);
  std::atomic_bool barrier(false);

  // Each thread will allocate and deallocate buffers in a tight loop.
  // Synchronize threads to start at the same time.
  auto thread_func = [&channel, &allocs_nr, &barrier, &thread_count]() {
    LOG(INFO) << "Thread " << std::this_thread::get_id() << " started.";
    std::vector<MachnetRingSlot_t> buffer_indices(kMaxAllocNr, UINT32_MAX);

    // Wait for parent to notify start of work.
    while (!barrier.load()) {
      machnet_pause();
    }

    LOG(INFO) << "Thread " << std::this_thread::get_id() << " running.";

    // Lambda function to release buffers.
    auto release_buffers = [&channel, &buffer_indices](uint32_t n) {
      if (n == 0) return;
      int retries = 8;
      uint32_t ret;
      do {
        ret =
            __machnet_channel_buf_free_bulk(channel, n, buffer_indices.data());
      } while (ret == 0 && retries-- > 0);
      EXPECT_EQ(ret, n);
    };

    for (size_t i = 0; i < allocs_nr.size(); i++) {
      uint32_t alloc_nr = allocs_nr[i];
      uint32_t alloced_nr = __machnet_channel_buf_alloc_bulk(
          channel, alloc_nr, buffer_indices.data(), nullptr);
      release_buffers(alloced_nr);
    }

    // Decrease thread count to indicate that this thread is done.
    --thread_count;
  };

  // Launch the worker threads.
  std::vector<std::thread> threads;
  for (size_t i = 0; i < kThreadsNr; i++) {
    threads.emplace_back(thread_func).detach();
  }

  // Sleep for 10ms to allow threads to spawn.
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // Notify threads to start work.
  barrier.store(true);

  // Wait for all threads to complete.
  while (thread_count.load() > 0) {
    machnet_pause();
  }

  // Check the buffer pool state.
  EXPECT_EQ(__machnet_channel_buffers_avail(channel), kChannelRingSize - 1);
  std::vector<MachnetRingSlot_t> buffers(kChannelRingSize - 1);
  // Dequeue all the buffers from the pool.
  auto ret = __machnet_channel_buf_alloc_bulk(channel, buffers.size(),
                                              buffers.data(), nullptr);
  EXPECT_EQ(ret, buffers.size());
  sort(buffers.begin(), buffers.end());

  // Craft a vector of expected buffer indices.
  std::vector<MachnetRingSlot_t> expected_buffers(kChannelRingSize - 1);
  std::iota(expected_buffers.begin(), expected_buffers.end(), 0);

  // Check equality.
  EXPECT_EQ(buffers, expected_buffers);

  // Destroy the channel (should succeed).
  __machnet_channel_destroy(channel, channel_size, &channel_fd, is_posix_shm,
                            channel_name.c_str());
  EXPECT_EQ(channel_fd, -1);
}

int main(int argc, char **argv) {
  ::google::InitGoogleLogging(argv[0]);
  testing::InitGoogleTest(&argc, argv);
  FLAGS_logtostderr = 1;

  int ret = RUN_ALL_TESTS();
  return ret;
}
