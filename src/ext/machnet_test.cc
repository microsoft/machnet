/**
 * @file  machnet_test.cc
 * @brief Implements the helper library's unit tests.
 */

#include "machnet.h"

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <utils.h>

#include <algorithm>
#include <random>
#include <unordered_set>

#include "machnet_private.h"

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

DEFINE_uint64(machnet_slots_nr, 1 << 9, "Number of slots in the Machnet ring.");
DEFINE_uint64(app_slots_nr, 1 << 9, "Number of slots in the app ring.");
DEFINE_uint64(buffers_nr, 1 << 13, "Number of message buffers available.");
DEFINE_uint64(buffer_size, 1 << 11, "Size of each message buffer.");

// Machnet channel context used
MachnetChannelCtx_t *g_channel_ctx = nullptr;

// Prepare a PRNG.
std::random_device rnd_device;
std::mt19937 mersenne_engine{rnd_device()};

/**
 * Helper function that given a message size, and a number of desired segments
 * produces a number of segment buffers for storage. The size of each segment is
 * pseudorandom, but all of the segments size add up to the requested message
 * size.
 *
 * @param msg_size           The requested message size.
 * @param segments_nr        # of segments wanted.
 * @param segments           The final vector that holds, properly sized (but
 *                           not initialized) segment buffers.
 */
void prepare_segments(uint32_t msg_size, uint32_t segments_nr,
                      std::vector<std::vector<uint8_t>> *segments) {
  CHECK_GT(msg_size, segments_nr)
      << "Message size must be greater than number of segments.";
  auto base = static_cast<int>(msg_size / segments_nr);

  std::vector<uint32_t> seg_sizes(segments_nr, base);
  uint32_t remainder = msg_size % segments_nr;
  for (uint32_t i = 0; i < remainder; i++) seg_sizes[i]++;

  // Note, this can produce zero-sized segments. This should be acceptable.
  std::uniform_int_distribution<int> seg_rnd{-base, base};
  for (uint32_t i = 0; i < segments_nr; i += 2) {
    if (i + 1 >= segments_nr) break;
    auto rnd_val = seg_rnd(mersenne_engine);

    seg_sizes[i] += rnd_val;
    seg_sizes[i + 1] -= rnd_val;
  }

  for (uint32_t i = 0; i < segments_nr; i++)
    segments->emplace_back(std::vector<uint8_t>(seg_sizes[i]));

  uint32_t total_size = 0;
  for (auto &seg : *segments) total_size += seg.size();
  CHECK_EQ(total_size, msg_size);
}

/**
 * Helper function that given a message (i.e. its segments), it initializes it
 * and  prepares all the metadata for transmission.
 *
 * @param flow_info          The flow information for the message.
 * @param tx_iov             A pointer to a vector of `MachnetIovec_t' objects.
 * @param tx_msghdr          A pointer to  an `MachnetMsgHdr_t' object.
 * @param tx_segments        A pointer to the message segments.
 * @param msg_size           Total message size.
 */
void prepare_tx_msg(MachnetFlow_t *flow_info,
                    std::vector<MachnetIovec_t> *tx_iov,
                    MachnetMsgHdr_t *tx_msghdr,
                    std::vector<std::vector<uint8_t>> *tx_segments,
                    uint32_t msg_size) {
  size_t counter = 0;
  for (size_t i = 0; i < tx_segments->size(); i++) {
    std::iota(tx_segments->at(i).begin(), tx_segments->at(i).end(), counter);
    counter += tx_segments->at(i).size();
  }

  tx_iov->resize(tx_segments->size());
  for (size_t i = 0; i < tx_segments->size(); i++) {
    tx_iov->at(i).base = tx_segments->at(i).data();
    tx_iov->at(i).len = tx_segments->at(i).size();
  }
  *flow_info = {.src_ip = UINT32_MAX,
                .dst_ip = UINT32_MAX,
                .src_port = UINT16_MAX,
                .dst_port = UINT16_MAX};
  tx_msghdr->msg_size = msg_size;
  tx_msghdr->flow_info = *flow_info;
  tx_msghdr->msg_iov = tx_iov->data();
  tx_msghdr->msg_iovlen = tx_iov->size();
  tx_msghdr->flags = -1;
}

/**
 * Helper function that prepares the metadata for receiving a message.
 *
 * @param rx_iov             A pointer to a vector of `MachnetIovec_t' objects.
 * @param rx_msghdr          A pointer to  an `MachnetMsgHdr_t' object.
 * @param rx_segments        A pointer to the buffer segments that will hold the
 *                           received message.
 * @param msg_size           Maximum message size that can be accommodated in
 *                           `rx_segments'.
 */
void prepare_rx_msg(std::vector<MachnetIovec_t> *rx_iov,
                    MachnetMsgHdr_t *rx_msghdr,
                    std::vector<std::vector<uint8_t>> *rx_segments,
                    uint32_t msg_size) {
  rx_iov->resize(rx_segments->size());
  for (size_t i = 0; i < rx_segments->size(); i++) {
    rx_iov->at(i).base = rx_segments->at(i).data();
    rx_iov->at(i).len = rx_segments->at(i).size();
  }

  rx_msghdr->msg_size = 0;
  rx_msghdr->flow_info = {
      .src_ip = 0, .dst_ip = 0, .src_port = 0, .dst_port = 0};
  rx_msghdr->msg_iov = rx_iov->data();
  rx_msghdr->msg_iovlen = rx_iov->size();
}

// This function bounces messages originally enqueud by the application to
// Machnet, back to the application.
uint32_t bounce_machnet_to_app(const MachnetChannelCtx_t *ctx) {
  std::vector<MachnetRingSlot_t> msgs;

  // Dequeue any messages from the application.
  msgs.resize(__machnet_channel_app_ring_pending(ctx));
  uint32_t ret =
      __machnet_channel_app_ring_dequeue(ctx, msgs.size(), msgs.data());
  if (ret != msgs.size())  // This should not happen.
    return -1;

  // Now bounce the dequeued messages back to the application.
  ret = __machnet_channel_machnet_ring_enqueue(ctx, msgs.size(), msgs.data());
  if (ret != msgs.size())  // This should not happen.
    return -1;

  return ret;
}

// Helper function that validates all the buffers in the pool.
// Returns true, if all the buffers are free and there are no duplicates.
// Could be called after each test round, to validate that the buffer pool is in
// a valid state.
bool check_buffer_pool(const MachnetChannelCtx_t *ctx) {
  // Release cached buffers to the pool
  if (__machnet_channel_buf_free_bulk(ctx, ctx->app_buffer_cache.count,
                                      ctx->app_buffer_cache.indices) !=
      ctx->app_buffer_cache.count) {
    return false;
  }
  *__DECONST(uint32_t *, &ctx->app_buffer_cache.count) = 0;

  jring_t *buf_ring = __machnet_channel_buf_ring(ctx);
  auto nbuffers = buf_ring->capacity;

  std::vector<MachnetRingSlot_t> buffers(nbuffers);

  // Dequeue all the buffers from the pool.
  if (__machnet_channel_buf_alloc_bulk(ctx, buffers.size(), buffers.data(),
                                       nullptr) != buffers.size()) {
    return false;
  }

  // Release all the buffers back to the pool.
  if (__machnet_channel_buf_free_bulk(ctx, buffers.size(), buffers.data()) !=
      buffers.size()) {
    return false;
  }

  std::unordered_set<MachnetRingSlot_t> s;

  for (auto &elem : buffers) {
    if (s.find(elem) != s.end()) return false;
    s.insert(elem);
  }
  return true;
}

int open_posix_channel(const char *channel_name) {
  int shm_fd = shm_open(channel_name, O_RDWR, 0666);
  if (shm_fd == -1) {
    perror("shm_open()");
    return -1;
  }

  return shm_fd;
}

TEST(MachnetTest, Bind) {
  // Trying to open a non-existent Machnet channel (should fail).
  const char *channel_name = "non_existent_machnet_channel_127";
  int shm_fd = open_posix_channel(channel_name);
  EXPECT_EQ(shm_fd, -1);
  size_t channel_size;
  MachnetChannelCtx_t *ctx = machnet_bind(shm_fd, &channel_size);
  EXPECT_EQ(ctx, nullptr);
  EXPECT_EQ(channel_size, 0);
  EXPECT_EQ(shm_fd, -1);

  // Create a POSIX shm channel.
  size_t expected_channel_size = __machnet_channel_dataplane_calculate_size(
      FLAGS_machnet_slots_nr, FLAGS_app_slots_nr, FLAGS_buffers_nr,
      FLAGS_buffer_size, 1);
  ctx = __machnet_channel_posix_create(channel_name, expected_channel_size,
                                       &shm_fd);
  EXPECT_NE(ctx, nullptr);
  EXPECT_GT(shm_fd, 0);

  int is_posix_shm = 1;
  __machnet_channel_destroy(ctx, expected_channel_size, &shm_fd, is_posix_shm,
                            channel_name);
}

TEST(MachnetTest, SimpleSendRecvMsg) {
  // Send a single-buffer message.
  const uint32_t buffer_payload =
      FLAGS_buffer_size - sizeof(MachnetMsgBuf_t) - MACHNET_MSGBUF_HEADROOM_MAX;
  std::vector<uint8_t> orig_data(buffer_payload);
  std::iota(orig_data.begin(), orig_data.end(), 0);

  // Prepare and send the message.
  MachnetIovec_t iov;
  iov.base = orig_data.data();
  iov.len = orig_data.size();
  MachnetMsgHdr_t msghdr;
  msghdr.flow_info = {.src_ip = UINT32_MAX,
                      .dst_ip = UINT32_MAX,
                      .src_port = UINT16_MAX,
                      .dst_port = UINT16_MAX};
  msghdr.msg_size = orig_data.size();
  msghdr.msg_iov = &iov;
  msghdr.msg_iovlen = 1;

  int ret = machnet_sendmsg(g_channel_ctx, &msghdr);
  EXPECT_EQ(ret, 0);
  EXPECT_EQ(__machnet_channel_app_ring_pending(g_channel_ctx), 1);

  // Bounce the message back to the application.
  EXPECT_EQ(bounce_machnet_to_app(g_channel_ctx), 1);
  EXPECT_EQ(__machnet_channel_machnet_ring_pending(g_channel_ctx), 1);

  // Receive the message and check its content.
  std::vector<uint8_t> recv_data(orig_data.size());
  iov.base = recv_data.data();
  iov.len = recv_data.size();
  msghdr.msg_size = 0;
  msghdr.flow_info = {.src_ip = 0, .dst_ip = 0, .src_port = 0, .dst_port = 0};
  EXPECT_EQ(machnet_recvmsg(g_channel_ctx, &msghdr), 1);
  EXPECT_EQ(msghdr.msg_size, orig_data.size());
  EXPECT_EQ(recv_data, orig_data);
  EXPECT_EQ(msghdr.flow_info.src_ip, UINT32_MAX);
  EXPECT_EQ(msghdr.flow_info.dst_ip, UINT32_MAX);
  EXPECT_EQ(msghdr.flow_info.src_port, UINT16_MAX);
  EXPECT_EQ(msghdr.flow_info.dst_port, UINT16_MAX);
  EXPECT_TRUE(check_buffer_pool(g_channel_ctx));
}

TEST(MachnetTest, MultiBufferSendRecvMsg) {
  // Prepare a generator for msg lengths.
  std::mt19937 mersenne_engine{rnd_device()};
  std::uniform_int_distribution<uint32_t> msg_len{0, MACHNET_MSG_MAX_LEN};

  const size_t nr_msgs = 256;
  for (size_t i = 0; i < nr_msgs; i++) {
    const uint32_t msg_size = msg_len(mersenne_engine);
    MachnetFlow_t flow;
    std::vector<MachnetIovec_t> tx_iov;
    MachnetMsgHdr_t tx_msghdr;
    std::vector<std::vector<uint8_t>> tx_msg_data;

    // Construct and send a message.
    prepare_segments(msg_size, 1, &tx_msg_data);
    CHECK_EQ(tx_msg_data.size(), 1);
    prepare_tx_msg(&flow, &tx_iov, &tx_msghdr, &tx_msg_data, msg_size);
    int ret = machnet_sendmsg(g_channel_ctx, &tx_msghdr);
    EXPECT_EQ(ret, 0) << "Msg size: " << msg_size;

    // Bounce the message back to the application.
    EXPECT_EQ(bounce_machnet_to_app(g_channel_ctx), 1);

    // Receive and inspect the message.
    std::vector<MachnetIovec_t> rx_iov;
    MachnetMsgHdr_t rx_msghdr;
    std::vector<std::vector<uint8_t>> rx_msg_data;

    prepare_segments(msg_size, 1, &rx_msg_data);
    prepare_rx_msg(&rx_iov, &rx_msghdr, &rx_msg_data, msg_size);
    ret = machnet_recvmsg(g_channel_ctx, &rx_msghdr);
    EXPECT_EQ(ret, 1) << "Msg size: " << msg_size;
    EXPECT_EQ(rx_msg_data, tx_msg_data) << "Msg size: " << msg_size;
    EXPECT_EQ(memcmp(&rx_msghdr.flow_info, &flow, sizeof(flow)), 0);
    EXPECT_TRUE(check_buffer_pool(g_channel_ctx)) << "Msg size: " << msg_size;
    EXPECT_EQ(jring_full(__machnet_channel_buf_ring(g_channel_ctx)), 1)
        << "Available buffers: "
        << jring_count(__machnet_channel_buf_ring(g_channel_ctx));
  }
}

TEST(MachnetTest, MultiBufferSGSendRecvMsg) {
  // Message length generator.
  std::uniform_int_distribution<uint32_t> msg_len{1, MACHNET_MSG_MAX_LEN};

  const size_t nr_msgs = 512;
  for (size_t i = 0; i < nr_msgs; i++) {
    const uint32_t segments_nr_max = 64;
    const uint32_t msg_size = msg_len(mersenne_engine);
    std::uniform_int_distribution<uint32_t> seg_nr{
        1, std::min(segments_nr_max, msg_size)};
    std::vector<std::vector<uint8_t>> tx_segments;
    prepare_segments(msg_size, seg_nr(mersenne_engine), &tx_segments);

    std::vector<MachnetIovec_t> tx_iov;
    MachnetFlow_t flow;
    MachnetMsgHdr_t tx_msghdr;

    // Send the msg.
    prepare_tx_msg(&flow, &tx_iov, &tx_msghdr, &tx_segments, msg_size);
    int ret = machnet_sendmsg(g_channel_ctx, &tx_msghdr);
    EXPECT_EQ(ret, 0) << "Msg size: " << msg_size;

    // Bounce the message back to the application.
    EXPECT_EQ(bounce_machnet_to_app(g_channel_ctx), 1);

    // Receive and inspect the message.
    std::vector<std::vector<uint8_t>> rx_segments;
    prepare_segments(msg_size, seg_nr(mersenne_engine), &rx_segments);
    std::vector<MachnetIovec_t> rx_iov;
    MachnetMsgHdr_t rx_msghdr;

    prepare_rx_msg(&rx_iov, &rx_msghdr, &rx_segments, msg_size);
    ret = machnet_recvmsg(g_channel_ctx, &rx_msghdr);
    EXPECT_EQ(ret, 1) << "Msg size: " << msg_size;

    auto flatten_msg = [](std::vector<std::vector<uint8_t>> *segments) {
      std::vector<uint8_t> v;
      for (auto &seg : *segments) {
        v.insert(v.end(), std::make_move_iterator(seg.begin()),
                 std::make_move_iterator(seg.end()));
      }
      return v;
    };

    std::vector<uint8_t> tx_msg(flatten_msg(&tx_segments));
    std::vector<uint8_t> rx_msg(flatten_msg(&rx_segments));
    EXPECT_EQ(rx_msg, tx_msg);
    EXPECT_EQ(memcmp(&rx_msghdr.flow_info, &flow, sizeof(flow)), 0);
    EXPECT_TRUE(check_buffer_pool(g_channel_ctx));
    EXPECT_EQ(jring_full(__machnet_channel_buf_ring(g_channel_ctx)), 1)
        << "Available buffers: "
        << jring_count(__machnet_channel_buf_ring(g_channel_ctx));
  }
}

TEST(MachnetTest, InvalidSendRecv) {
  // Message length generator.
  std::uniform_int_distribution<uint32_t> invalid_msg_len{
      MACHNET_MSG_MAX_LEN + 1, 2 * MACHNET_MSG_MAX_LEN};

  const size_t nr_msgs = 256;
  for (size_t i = 0; i < nr_msgs; i++) {
    const uint32_t segments_nr_max = 64;
    const uint32_t msg_size = invalid_msg_len(mersenne_engine);
    std::uniform_int_distribution<uint32_t> seg_nr{
        1, std::min(segments_nr_max, msg_size)};
    std::vector<std::vector<uint8_t>> tx_segments;
    prepare_segments(msg_size, seg_nr(mersenne_engine), &tx_segments);

    std::vector<MachnetIovec_t> tx_iov;
    MachnetFlow_t flow;
    MachnetMsgHdr_t tx_msghdr;

    // Send the msg.
    prepare_tx_msg(&flow, &tx_iov, &tx_msghdr, &tx_segments, msg_size);
    int ret = machnet_sendmsg(g_channel_ctx, &tx_msghdr);
    EXPECT_EQ(ret, -1) << "Msg size: " << msg_size;
    EXPECT_TRUE(check_buffer_pool(g_channel_ctx));
  }

  std::uniform_int_distribution<uint32_t> msg_len{1, MACHNET_MSG_MAX_LEN};
  for (size_t i = 0; i < nr_msgs; i++) {
    const uint32_t segments_nr_max = 64;
    const uint32_t msg_size = msg_len(mersenne_engine);
    std::uniform_int_distribution<uint32_t> seg_nr{
        1, std::min(segments_nr_max, msg_size)};
    std::vector<std::vector<uint8_t>> tx_segments;
    prepare_segments(msg_size, seg_nr(mersenne_engine), &tx_segments);

    std::vector<MachnetIovec_t> tx_iov;
    MachnetFlow_t flow;
    MachnetMsgHdr_t tx_msghdr;

    // Send the msg.
    prepare_tx_msg(&flow, &tx_iov, &tx_msghdr, &tx_segments, msg_size);
    int ret = machnet_sendmsg(g_channel_ctx, &tx_msghdr);
    EXPECT_EQ(ret, 0) << "Msg size: " << msg_size;

    // Bounce the message back to the application.
    EXPECT_EQ(bounce_machnet_to_app(g_channel_ctx), 1);

    // Try to receive the message.
    std::vector<std::vector<uint8_t>> rx_segments;
    // NOTE: We request a smaller total buffer space, so that msg receive
    // fails.
    prepare_segments(msg_size / 2, seg_nr(mersenne_engine), &rx_segments);
    std::vector<MachnetIovec_t> rx_iov;
    MachnetMsgHdr_t rx_msghdr;

    prepare_rx_msg(&rx_iov, &rx_msghdr, &rx_segments, msg_size / 2);
    ret = machnet_recvmsg(g_channel_ctx, &rx_msghdr);
    EXPECT_EQ(ret, -1) << "Msg size: " << msg_size;
    EXPECT_TRUE(check_buffer_pool(g_channel_ctx));
  }
}

int main(int argc, char **argv) {
  ::google::InitGoogleLogging(argv[0]);
  testing::InitGoogleTest(&argc, argv);
  FLAGS_logtostderr = 1;

  // Create and initialize the channel.
  size_t channel_size;
  int is_posix_shm;
  int channel_fd;
  g_channel_ctx = __machnet_channel_create(channel_name, FLAGS_machnet_slots_nr,
                                           FLAGS_app_slots_nr, FLAGS_buffers_nr,
                                           FLAGS_buffer_size, &channel_size,
                                           &is_posix_shm, &channel_fd);
  if (g_channel_ctx == nullptr) return -1;

  int ret = RUN_ALL_TESTS();

  // Destroy the channel.
  __machnet_channel_destroy(g_channel_ctx, channel_size, &channel_fd,
                            is_posix_shm, channel_name);

  return ret;
}
