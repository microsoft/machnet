#include <benchmark/benchmark.h>
#include <machnet.h>
#include <machnet_private.h>
#include <utils.h>

#include <iostream>
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
const char *channel_name = file_name(__FILE__);
const uint32_t kRingSlotEntries = 1 << 16;
const uint32_t kPacketMTU = 1500;
const uint32_t kBufferSize = kPacketMTU + MACHNET_MSGBUF_SPACE_RESERVED;

static void BM_memcpy(benchmark::State &state) {  // NOLINT
  std::vector<char> src(state.range(0), 'a');
  std::vector<char> dst(state.range(0), 'b');
  for (auto _ : state)
    juggler::utils::Copy(dst.data(), src.data(), state.range(0));
  // state.SetBytesProcessed(int64_t(state.iterations()) *
  //                         int64_t(state.range(0)));
  auto bytes_cnt = state.iterations() * state.range(0);
  state.counters["bps"] =
      benchmark::Counter(bytes_cnt * 8, benchmark::Counter::kIsRate);
}
BENCHMARK(BM_memcpy)->RangeMultiplier(2)->Range(8, MACHNET_MSG_MAX_LEN);

static void BM_memcpy_llc_thrash(benchmark::State &state) {  // NOLINT
  const size_t kMaxMem = 1 << 28;
  const size_t kUnitSize = sizeof(uint32_t);
  const size_t kBlockSize = state.range(0);
  const size_t kNumUnitsPerBlock = kBlockSize / kUnitSize;
  const size_t kElemNr = kMaxMem / kBlockSize;
  CHECK_LE(kUnitSize, kBlockSize);

  std::vector<uint32_t> index_vec(kElemNr);
  std::iota(index_vec.begin(), index_vec.end(), 0);
  std::shuffle(index_vec.begin(), index_vec.end(),
               std::mt19937{std::random_device{}()});  // NOLINT

  std::vector<std::vector<uint32_t>> src_data(kElemNr);
  std::vector<std::vector<uint32_t>> dst_data(kElemNr);

  for (uint32_t i = 0; i < kElemNr; ++i) {
    src_data[i].resize(kNumUnitsPerBlock);
    dst_data[i].resize(kNumUnitsPerBlock);
    std::fill(src_data[i].begin(), src_data[i].end(), i);
    // The first element is used to indicate the next block to copy.
    // This would cause the worst case as the prefetcher won't be effective.
    src_data[i][0] = index_vec[i];
  }

  for (auto _ : state) {
    uint32_t index = 0;
    for (uint32_t i = 0; i < kElemNr; ++i) {
      juggler::utils::Copy(reinterpret_cast<uint8_t *>(dst_data[index].data()),
                           reinterpret_cast<uint8_t *>(src_data[index].data()),
                           kBlockSize);
      index = src_data[index][0];
    }
  }
  auto bytes_cnt = state.iterations() * kBlockSize * kElemNr;
  state.counters["bps"] =
      benchmark::Counter(bytes_cnt * 8, benchmark::Counter::kIsRate);
}
BENCHMARK(BM_memcpy_llc_thrash)
    ->RangeMultiplier(2)
    ->Range(8, MACHNET_MSG_MAX_LEN);

static void BM_machnet_sendmsg(benchmark::State &state) {  // NOLINT
  // Create and initialize the channel.
  size_t channel_size;
  int is_posix_shm;
  int channel_fd;
  MachnetChannelCtx_t *channel_ctx = __machnet_channel_create(
      channel_name, kRingSlotEntries, kRingSlotEntries, kRingSlotEntries,
      kBufferSize, &channel_size, &is_posix_shm, &channel_fd);
  if (channel_ctx == nullptr) {
    state.SkipWithError("Failed to create channel.");
    return;
  }

  // Prepare a message to send.
  std::vector<char> data(state.range(0));
  // std::vector<char> data(s*1e6);
  MachnetIovec_t iov;
  iov.base = data.data();
  iov.len = data.size();
  MachnetMsgHdr_t msghdr;
  msghdr.msg_size = data.size();
  msghdr.flags = 0;
  msghdr.flow_info = {.src_ip = UINT32_MAX,
                      .dst_ip = UINT32_MAX,
                      .src_port = UINT16_MAX,
                      .dst_port = UINT16_MAX};
  msghdr.msg_iov = &iov;
  msghdr.msg_iovlen = 1;

  // Temporary buffer to use for resetting rings.
  std::vector<MachnetRingSlot_t> slots(kRingSlotEntries - 1);

  // Lambda function to reset the buffer pool when the benchmark exhausts it.
  auto bufpool_reset = [&channel_ctx, &slots]() {
    // We need to drain the MACHNET application ring.
    uint32_t msg_pending = __machnet_channel_app_ring_pending(channel_ctx);

    CHECK_EQ(__machnet_channel_app_ring_dequeue(channel_ctx, msg_pending,
                                                slots.data()),
             msg_pending);

    // Next reset the buffer pool.
    // The Machnet channel initialized the buffer pool with buffer
    // indexes from [0, capacity]. Since there is no API to perform this
    // operation we use this quick hack.
    uint32_t nbuffers_free = __machnet_channel_buffers_avail(channel_ctx);
    CHECK_EQ(__machnet_channel_buf_alloc_bulk(channel_ctx, nbuffers_free,
                                              slots.data(), nullptr),
             nbuffers_free);

    std::iota(slots.begin(), slots.end(), 0);
    CHECK_EQ(slots.size(), __machnet_channel_buf_free_bulk(
                               channel_ctx, slots.size(), slots.data()));
  };

  for (auto _ : state) {
    for (uint32_t i = 0; i < state.range(1); i++) {
      int ret = machnet_sendmsg(channel_ctx, &msghdr);
      if (ret != 0) {
        state.SkipWithError("Failed to send message.");
        break;
      }
    }
    state.PauseTiming();
    bufpool_reset();
    state.ResumeTiming();
  }

  // Destroy the channel.
  __machnet_channel_destroy(channel_ctx, channel_size, &channel_fd,
                            is_posix_shm, channel_name);

  auto msg_cnt = state.iterations() * state.range(1);
  auto bytes_cnt = msg_cnt * state.range(0);
  // state.counters["msg_cnt"] = msg_cnt;
  // state.counters["msg_cnt"] = msg_cnt;
  state.counters["msg_rate"] =
      benchmark::Counter(msg_cnt, benchmark::Counter::kIsRate);
  state.counters["bps"] =
      benchmark::Counter(bytes_cnt * 8, benchmark::Counter::kIsRate);
}

static void CustomArguments(benchmark::internal::Benchmark *b) {
  for (uint32_t msg_sz = 8; msg_sz <= MACHNET_MSG_MAX_LEN; msg_sz *= 2) {
    // Buffers required for such message size.
    uint32_t buffer_nr = (msg_sz + kPacketMTU - 1) / kPacketMTU;
    // Maximum number of messages to send.
    uint32_t msg_nr = (kRingSlotEntries - 1) / buffer_nr;
    b->Args({msg_sz, msg_nr});
  }
}

// Register the function as a benchmark
// BENCHMARK(BM_machnet_sendmsg)->Range(4, 1500);
BENCHMARK(BM_machnet_sendmsg)->Apply(CustomArguments);

// Run the benchmark
BENCHMARK_MAIN();
