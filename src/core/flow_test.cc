/**
 * @file flow_test.cc
 *
 * Unit tests for the flow abstraction.
 */
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <unordered_set>
#include <vector>

#include "channel.h"
#include "machnet.h"
#include "machnet_pkthdr.h"
#include "packet_pool.h"
#include "pmd.h"

#define private public
#include "flow.h"

namespace juggler {
namespace net {
namespace flow {

/**
 * @class FlowTest
 * @brief Fixture for testing classes in the `flow' namespace.
 */
class FlowTest : public ::testing::Test {
 protected:
  inline static const char *kLocalIp = "10.0.0.1";
  const uint16_t kLocalPort = 1234;
  inline static const char *kRemoteIp = "10.0.0.2";
  const uint16_t kRemotePort = 888;
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
  const uint32_t kChannelRingSize = 1
                                    << 8;  // 256 slots for Machnet, App rings.
  const uint32_t kBufferRingSize = 1 << 13;  // 8K Buffers.
  const uint32_t kBufferSize = 1 << 11;      // 2KB for each buffer.
  const uint32_t kMbufsNr = 1 << 13;         // 8K mbufs.

  FlowTest()
      : rng_(std::random_device{}()),  // NOLINT
        channel_mgr_(),
        channel_(nullptr),
        tx_tracking_(nullptr) {
    local_addr_.FromString(kLocalIp);
    local_port_.port = be16_t(kLocalPort);
    remote_addr_.FromString(kRemoteIp);
    remote_port_.port = be16_t(kRemotePort);
  }

  void SetUp() override {
    CHECK_EQ(channel_mgr_.AddChannel(fname, kChannelRingSize, kChannelRingSize,
                                     kBufferRingSize, kBufferSize),
             true);
    channel_ = channel_mgr_.GetChannel(fname);
    tx_tracking_ = std::make_unique<TXTracking>(CHECK_NOTNULL(channel_.get()));
    rx_tracking_ = std::make_unique<RXTracking>(
        local_addr_.address.value(), local_port_.port.value(),
        remote_addr_.address.value(), remote_port_.port.value(),
        CHECK_NOTNULL(channel_.get()));
    pkt_pool_ = std::make_unique<dpdk::PacketPool>(
        kMbufsNr, dpdk::PmdRing::kDefaultFrameSize + RTE_ETHER_HDR_LEN +
                      RTE_ETHER_CRC_LEN + RTE_PKTMBUF_HEADROOM);
  }

  void TearDown() override {
    pkt_pool_.reset();
    rx_tracking_.reset();
    tx_tracking_.reset();
    channel_.reset();
    channel_mgr_.DestroyChannel(fname);
  }

  /**
   * @brief Create an 'Machnet' message buffer train to hold the content of
   * data.
   * @param data The data to be copied into the message buffer train.
   * @return A pointer to the first message buffer in the train.
   * @attention The caller is responsible to make sure that there are enough
   * buffers in the channel to accommodate the data length.
   */
  shm::MsgBuf *CreateMsg(const std::vector<uint8_t> &data) {
    const auto msgbuf_size = channel_->GetUsableBufSize();
    auto num_msgbufs = (data.size() + msgbuf_size - 1) / msgbuf_size;
    shm::MsgBuf *head = nullptr;
    shm::MsgBuf *tail = nullptr;
    size_t data_offset = 0;
    while (num_msgbufs) {
      shm::MsgBufBatch batch;
      auto msgbuf_nr =
          std::min(num_msgbufs, static_cast<size_t>(batch.GetRoom()));
      CHECK(channel_->MsgBufBulkAlloc(&batch, msgbuf_nr));

      for (auto i = 0; i < batch.GetSize(); i++) {
        auto *msgbuf = batch[i];
        auto nbytes_to_copy = std::min(static_cast<size_t>(msgbuf_size),
                                       data.size() - data_offset);
        utils::Copy(CHECK_NOTNULL(msgbuf->append(nbytes_to_copy)),
                    &data[data_offset], nbytes_to_copy);
        data_offset += nbytes_to_copy;
        if (head == nullptr) {
          head = msgbuf;
          tail = msgbuf;
        } else {
          tail->set_next(msgbuf);
          tail = msgbuf;
        }
      }

      num_msgbufs -= msgbuf_nr;
      batch.Clear();
    }
    head->set_msg_length(data.size());
    head->set_last(tail->index());
    head->mark_first();
    tail->mark_last();

    return head;
  }

  /**
   * @brief Given a message buffer, construct a train of Machnet packets. This
   * emulates the  Machnet network packetization process, and is used to test
   * the receiving end of the flow.
   *
   * @param pcb  The flow's protocol control block (PCB), that contains the
   * relevant state that is going to be used for the packetization (e.g.,
   * sequence number).
   * @param data The source data to be packetized.
   * @return A vector containing the networking packets corresponding to the
   * data.
   */
  std::vector<dpdk::Packet *> CreatePacketTrain(
      swift::Pcb *pcb, const std::vector<uint8_t> &data) {
    const auto max_packet_size = dpdk::PmdRing::kDefaultFrameSize;
    constexpr auto packet_hdr_size = sizeof(net::Ethernet) + sizeof(net::Ipv4) +
                                     sizeof(net::Udp) +
                                     sizeof(net::MachnetPktHdr);
    const auto max_payload_size = max_packet_size - packet_hdr_size;
    auto num_packets = (data.size() + max_payload_size - 1) / max_payload_size;

    std::vector<dpdk::Packet *> packets(num_packets, nullptr);

    // Fail if there are not enough packets in the pool.
    CHECK(pkt_pool_->PacketBulkAlloc(packets.data(), num_packets));

    size_t data_offset = 0;
    for (auto &packet : packets) {
      const auto max_packet_payload_size = max_packet_size - packet_hdr_size;

      auto payload_size =
          std::min(max_packet_payload_size, data.size() - data_offset);
      auto *eh = CHECK_NOTNULL(
          packet->append<net::Ethernet *>(payload_size + packet_hdr_size));

      // We do not need to fill in the Ethernet, Ipv4, and UDP headers, as
      // they are not used by the RXQeueue.
      auto *ipv4 = reinterpret_cast<net::Ipv4 *>(eh + 1);
      auto *udph = reinterpret_cast<net::Udp *>(ipv4 + 1);
      auto *machneth = reinterpret_cast<net::MachnetPktHdr *>(udph + 1);
      machneth->magic = be16_t(net::MachnetPktHdr::kMagic);
      machneth->net_flags = net::MachnetPktHdr::MachnetFlags::kData;
      machneth->seqno = be32_t(pcb->snd_nxt++);
      machneth->msg_flags = MACHNET_MSGBUF_FLAGS_SG;
      if (data_offset == 0) {
        machneth->msg_flags |= MACHNET_MSGBUF_FLAGS_SYN;
      }
      if (data_offset + payload_size == data.size()) {
        machneth->msg_flags |= MACHNET_MSGBUF_FLAGS_FIN;
        machneth->msg_flags &= ~MACHNET_MSGBUF_FLAGS_SG;
      }
      auto *payload = reinterpret_cast<uint8_t *>(machneth + 1);
      utils::Copy(payload, &data[data_offset], payload_size);

      // Update the data offset.
      data_offset += payload_size;
    }

    return packets;
  }

  Ipv4::Address local_addr_;
  Udp::Port local_port_;
  Ipv4::Address remote_addr_;
  Udp::Port remote_port_;
  std::mt19937 rng_;
  shm::ChannelManager<shm::Channel> channel_mgr_;
  std::shared_ptr<shm::Channel> channel_;
  std::unique_ptr<TXTracking> tx_tracking_;
  std::unique_ptr<RXTracking> rx_tracking_;
  std::unique_ptr<dpdk::PacketPool> pkt_pool_;
};

TEST_F(FlowTest, TXQueue_init) {
  EXPECT_EQ(tx_tracking_->NumUnsentMsgbufs(), 0);
  EXPECT_EQ(tx_tracking_->NumTrackedMsgbufs(), 0);
  EXPECT_EQ(tx_tracking_->GetOldestUnackedMsgBuf(), nullptr);
  EXPECT_EQ(tx_tracking_->GetOldestUnsentMsgBuf(), nullptr);
  EXPECT_EQ(tx_tracking_->GetLastMsgBuf(), nullptr);
}

TEST_F(FlowTest, TXQueue_PushPop) {
  std::mt19937 engine(rng_);
  std::uniform_int_distribution<std::mt19937::result_type> dist(
      1, MACHNET_MSG_MAX_LEN);
  auto msg_len = dist(rng_);
  auto buffers_nr = (msg_len + channel_->GetUsableBufSize() - 1) /
                    channel_->GetUsableBufSize();
  uint32_t total_buffers_nr = 0;
  shm::MsgBuf *first = nullptr;
  shm::MsgBuf *last = nullptr;
  while (buffers_nr < channel_->GetFreeBufCount()) {
    std::vector<uint8_t> data(msg_len);
    std::generate(data.begin(), data.end(), std::rand);
    auto *msgbuf = CreateMsg(data);
    if (first == nullptr) first = msgbuf;
    last = channel_->GetMsgBuf(msgbuf->last());
    tx_tracking_->Append(msgbuf);
    total_buffers_nr += buffers_nr;
    EXPECT_EQ(tx_tracking_->NumUnsentMsgbufs(), total_buffers_nr);
    EXPECT_EQ(tx_tracking_->NumTrackedMsgbufs(), total_buffers_nr);
    EXPECT_EQ(tx_tracking_->GetOldestUnackedMsgBuf(), first);
    EXPECT_EQ(tx_tracking_->GetLastMsgBuf(),
              channel_->GetMsgBuf(msgbuf->last()));

    msg_len = dist(rng_);
    buffers_nr = (msg_len + channel_->GetUsableBufSize() - 1) /
                 channel_->GetUsableBufSize();
  }

  auto msgbuf = first;
  for (uint32_t i = 0; i < total_buffers_nr; i++) {
    EXPECT_EQ(tx_tracking_->NumTrackedMsgbufs(), total_buffers_nr);
    EXPECT_EQ(tx_tracking_->NumUnsentMsgbufs(), total_buffers_nr - i);
    auto buf = tx_tracking_->GetAndUpdateOldestUnsent();
    EXPECT_TRUE(buf.has_value());
    EXPECT_EQ(buf.value(), msgbuf);
    if (msgbuf->has_next() || msgbuf->has_chain())
      msgbuf = channel_->GetMsgBuf(msgbuf->next());
    else
      msgbuf = nullptr;
    EXPECT_EQ(tx_tracking_->GetOldestUnackedMsgBuf(), first);
    EXPECT_EQ(tx_tracking_->GetOldestUnsentMsgBuf(), msgbuf);
    EXPECT_EQ(tx_tracking_->GetLastMsgBuf(), last);
    EXPECT_EQ(tx_tracking_->NumUnsentMsgbufs(), total_buffers_nr - i - 1);
    EXPECT_EQ(tx_tracking_->NumTrackedMsgbufs(), total_buffers_nr);
  }

  auto buf = tx_tracking_->GetAndUpdateOldestUnsent();
  EXPECT_EQ(buf.has_value(), false);
  EXPECT_EQ(tx_tracking_->GetOldestUnsentMsgBuf(), nullptr);

  tx_tracking_->ReceiveAcks(total_buffers_nr);
  EXPECT_EQ(tx_tracking_->NumUnsentMsgbufs(), 0);
  EXPECT_EQ(tx_tracking_->NumTrackedMsgbufs(), 0);
  EXPECT_EQ(channel_->GetFreeBufCount(), channel_->GetTotalBufCount());
}

TEST_F(FlowTest, RXQueue_init) {
  EXPECT_EQ(rx_tracking_->ReassemblyQueueCapacity(),
            RXTracking::kReassemblyQueueDefaultSize - 1);
}

TEST_F(FlowTest, RXQueue_Push) {
  std::mt19937 engine(rng_);
  std::uniform_int_distribution<std::mt19937::result_type> dist(
      1, MACHNET_MSG_MAX_LEN);
  const size_t kNumTries = 1;
  for (size_t i = 0; i < kNumTries; i++) {
    // Fill a message with random data.
    auto msg_len = dist(rng_);
    std::vector<uint8_t> tx_message(msg_len);
    std::generate(tx_message.begin(), tx_message.end(), std::rand);

    // Create a Protocol Control Block.
    swift::Pcb tx_pcb;

    // Create a packet train from the message.
    auto packets(CreatePacketTrain(&tx_pcb, tx_message));

    swift::Pcb rx_pcb;
    auto buffers_used = 0;
    // Push the packets into the RX queue.
    for (auto &pkt : packets) {
      auto prev_rcv_nxt = rx_pcb.get_rcv_nxt();
      rx_tracking_->Push(&rx_pcb, pkt);
      // Each packet corresponds to a single channel buffer.
      buffers_used++;
      EXPECT_EQ(channel_->GetFreeBufCount(),
                channel_->GetTotalBufCount() - buffers_used);
      // All packets are in order so the reassembly queue should be empty.
      EXPECT_TRUE(rx_tracking_->IsReassemblyQueueEmpty());
      EXPECT_EQ(rx_pcb.get_rcv_nxt(), prev_rcv_nxt + 1);
    }

    // At this point the message should have been delivered to the application.
    std::vector<uint8_t> rx_message(msg_len);
    MachnetIovec_t rx_iov;
    rx_iov.base = rx_message.data();
    rx_iov.len = rx_message.size();
    MachnetMsgHdr_t rx_msghdr;
    rx_msghdr.flags = 0;
    rx_msghdr.flow_info = {0, 0, 0, 0};
    rx_msghdr.msg_iov = &rx_iov;
    rx_msghdr.msg_iovlen = 1;
    auto ret = machnet_recvmsg(channel_->ctx(), &rx_msghdr);
    EXPECT_EQ(ret, 1) << "Failed to deliver message to application";
    EXPECT_EQ(tx_message, rx_message);
    EXPECT_EQ(channel_->GetFreeBufCount(), channel_->GetTotalBufCount());

    // Release packets.
    for (auto &pkt : packets) {
      dpdk::Packet::Free(pkt);
    }
  }
}

TEST_F(FlowTest, RXQueue_Push_OutOfOrder1) {
  constexpr auto packet_hdr_size = sizeof(net::Ethernet) + sizeof(net::Ipv4) +
                                   sizeof(net::Udp) +
                                   sizeof(net::MachnetPktHdr);
  const auto packet_payload_size =
      dpdk::PmdRing::kDefaultFrameSize - packet_hdr_size;
  std::mt19937 engine(rng_);
  // The lower bound of the message size should span at least two packets, so we
  // can test for out-of-order.
  std::uniform_int_distribution<> dist(
      2 * packet_payload_size,
      rx_tracking_->ReassemblyQueueCapacity() * packet_payload_size);

  const size_t kNumTries = 1000;
  for (size_t i = 0; i < kNumTries; i++) {
    // Fill a message with random data.
    auto msg_len = dist(rng_);
    std::vector<uint8_t> tx_message(msg_len);
    std::generate(tx_message.begin(), tx_message.end(), std::rand);

    // Create a Protocol Control Block.
    swift::Pcb tx_pcb;

    // Create a packet train from the message.
    auto packets(CreatePacketTrain(&tx_pcb, tx_message));
    std::reverse(packets.begin(), packets.end());
    // std::shuffle(packets.begin(), packets.end(), rng_);

    // Create a Protocol Control Block.
    swift::Pcb rx_pcb;
    auto buffers_used = 0;
    // Push the packets into the RX queue.
    for (auto &pkt : packets) {
      auto prev_rcv_nxt = rx_pcb.get_rcv_nxt();
      rx_tracking_->Push(&rx_pcb, pkt);
      // Each packet corresponds to a single channel buffer.
      buffers_used++;
      EXPECT_EQ(channel_->GetFreeBufCount(),
                channel_->GetTotalBufCount() - buffers_used);
      // All packets are in order so the reassembly queue should be empty.
      if (pkt == packets.back()) {
        EXPECT_TRUE(rx_tracking_->IsReassemblyQueueEmpty());
        // `rcv_nxt` should now be updated; the last packet fills the final gap.
        EXPECT_EQ(prev_rcv_nxt + buffers_used, rx_pcb.get_rcv_nxt());
        // The message should have now been delivered to the application, and
        // the reassembly queue should be empty.
        EXPECT_TRUE(rx_tracking_->IsReassemblyQueueEmpty());
        EXPECT_EQ(rx_pcb.sack_bitmap_count, 0);
      } else {
        // All packets are pushed to the rassembly queue out-of-order.
        EXPECT_FALSE(rx_tracking_->IsReassemblyQueueEmpty());
        // rcv_nxt should not be updated.
        EXPECT_EQ(prev_rcv_nxt, rx_pcb.get_rcv_nxt());
        // The reassembly queue size should increase with each packet pushed.
        EXPECT_EQ(rx_pcb.sack_bitmap_count, buffers_used);
      }
    }

    // At this point the message should have been delivered to the application.
    std::vector<uint8_t> rx_message(msg_len);
    MachnetIovec_t rx_iov;
    rx_iov.base = rx_message.data();
    rx_iov.len = rx_message.size();
    MachnetMsgHdr_t rx_msghdr;
    rx_msghdr.flags = 0;
    rx_msghdr.flow_info = {0, 0, 0, 0};
    rx_msghdr.msg_iov = &rx_iov;
    rx_msghdr.msg_iovlen = 1;
    auto ret = machnet_recvmsg(channel_->ctx(), &rx_msghdr);
    EXPECT_EQ(ret, 1) << "Failed to deliver message to application";
    EXPECT_EQ(tx_message, rx_message);
    EXPECT_EQ(channel_->GetFreeBufCount(), channel_->GetTotalBufCount());

    // Release packets.
    for (auto &pkt : packets) {
      dpdk::Packet::Free(pkt);
    }
  }
}

/**
 * @brief This is a test for the RX queue's Push() method with out-of-order
 * packets. It is similar to RXQueue_Push_OutOfOrder1, but more rigorous in that
 * it tests multiple, random-length out-of-order batches within a single
 * message. The test used the maximum message size to generate a large amount of
 * out-of-order batches and is repeated multiple times.
 */
TEST_F(FlowTest, RXQueue_Push_OutOfOrder2) {
  const auto msg_len = MACHNET_MSG_MAX_LEN;

  std::mt19937 engine(rng_);

  const size_t kNumTries = 50;
  for (size_t i = 0; i < kNumTries; i++) {
    // Fill a message with random data.
    std::vector<uint8_t> tx_message(msg_len);
    std::generate(tx_message.begin(), tx_message.end(), std::rand);

    // Create a Protocol Control Block.
    swift::Pcb tx_pcb;

    // Create a packet train from the message.
    std::vector<dpdk::Packet *> packets(CreatePacketTrain(&tx_pcb, tx_message));
    size_t index = 0;
    // This set contains all packet indices that mark the end of an out-of-order
    // batch. Upon pushing this packet to the RX queue, the reassembly queue
    // should be flushed because all gaps are filled till this point.
    std::unordered_set<size_t> indices;
    while (index < packets.size()) {
      std::uniform_int_distribution<size_t> dist(
          2, rx_tracking_->ReassemblyQueueCapacity());

      auto ooo_batch_size = std::min(dist(rng_), packets.size() - index);
      std::shuffle(packets.begin() + index,
                   packets.begin() + index + ooo_batch_size, rng_);
      index += ooo_batch_size;
      indices.insert(index);
    }

    // Create a Protocol Control Block.
    swift::Pcb rx_pcb;
    auto buffers_used = 0;
    auto prev_rcv_nxt = rx_pcb.get_rcv_nxt();
    // Push the packets into the RX queue.
    for (auto &pkt : packets) {
      rx_tracking_->Push(&rx_pcb, pkt);
      // Each packet corresponds to a single channel buffer.
      buffers_used++;
      EXPECT_EQ(channel_->GetFreeBufCount(),
                channel_->GetTotalBufCount() - buffers_used);
      if (pkt == packets.back()) {
        // For the last packet, the reassembly queue should be empty.
        EXPECT_TRUE(rx_tracking_->IsReassemblyQueueEmpty());
        // rcv_nxt should now be updated; the last packet fills the final gap.
        EXPECT_EQ(prev_rcv_nxt + buffers_used, rx_pcb.get_rcv_nxt());
        // The message should have now been delivered to the application, and
        // the reassembly queue should be empty.
        EXPECT_TRUE(rx_tracking_->IsReassemblyQueueEmpty());
        EXPECT_EQ(rx_pcb.sack_bitmap_count, 0);
      } else {
        if (indices.find(buffers_used) != indices.end()) {
          // For the last packet in an out-of-order batch, the reassembly queue
          // should be flushed, and `rcv_nxt` should be updated.
          EXPECT_TRUE(rx_tracking_->IsReassemblyQueueEmpty());
          // `rcv_nxt` should be updated here.
          EXPECT_EQ(prev_rcv_nxt + buffers_used, rx_pcb.get_rcv_nxt());
          EXPECT_EQ(rx_pcb.sack_bitmap_count, 0);
        }
        // The amount of buffers being used should always be equal to the sum of
        // the number of packets in the reassembly queue plus the increments in
        // `rcv_nxt`.
        EXPECT_EQ(
            rx_pcb.get_rcv_nxt() + rx_pcb.sack_bitmap_count - prev_rcv_nxt,
            buffers_used);
      }
    }

    // At this point the message should have been delivered to the application.
    std::vector<uint8_t> rx_message(msg_len);
    MachnetIovec_t rx_iov;
    rx_iov.base = rx_message.data();
    rx_iov.len = rx_message.size();
    MachnetMsgHdr_t rx_msghdr;
    rx_msghdr.flags = 0;
    rx_msghdr.flow_info = {0, 0, 0, 0};
    rx_msghdr.msg_iov = &rx_iov;
    rx_msghdr.msg_iovlen = 1;
    auto ret = machnet_recvmsg(channel_->ctx(), &rx_msghdr);
    EXPECT_EQ(ret, 1) << "Failed to deliver message to application";
    EXPECT_EQ(tx_message, rx_message);
    EXPECT_EQ(channel_->GetFreeBufCount(), channel_->GetTotalBufCount());

    // Release packets.
    for (auto &pkt : packets) {
      dpdk::Packet::Free(pkt);
    }
  }
}

}  // namespace flow
}  // namespace net
}  // namespace juggler

int main(int argc, char **argv) {
  ::google::InitGoogleLogging(argv[0]);
  testing::InitGoogleTest(&argc, argv);

  // Initialize DPDK
  auto kEalOpts = juggler::utils::CmdLineOpts(
      {"-c", "0x0", "-n", "6", "--proc-type=auto", "-m", "1024", "--log-level",
       "8", "--vdev=net_null0,copy=1", "--no-pci"});
  auto d = juggler::dpdk::Dpdk();
  d.InitDpdk(kEalOpts); /* Using default DPDK configurations */

  return RUN_ALL_TESTS();
}
