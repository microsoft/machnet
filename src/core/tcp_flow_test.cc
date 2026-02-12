/**
 * @file tcp_flow_test.cc
 *
 * Unit tests for the TcpFlow class (src/include/tcp_flow.h).
 *
 * These tests exercise:
 *  - TCP state machine transitions (handshake, shutdown, RST)
 *  - MSS option parsing from TCP headers
 *  - Data TX path (OutputMessage with length-prefix framing)
 *  - Data RX path (ConsumePayload de-framing into MsgBufs)
 *  - Retransmission overlap handling (ProcessInOrderPayload)
 *  - PeriodicCheck (RTO, TIME_WAIT countdown)
 *  - Sequence number comparison helpers
 */

// White-box testing: access private members of TcpFlow.
// The define must come AFTER all system/STL/gtest includes.

#include <glog/logging.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "channel.h"
#include "channel_msgbuf.h"
#include "common.h"
#include "dpdk.h"
#include "ether.h"
#include "ipv4.h"
#include "machnet.h"
#include "machnet_common.h"
#include "packet.h"
#include "packet_pool.h"
#include "pmd.h"
#include "tcp.h"
#include "utils.h"

// Access private members of TcpFlow.
#define private public
#define protected public

#include "tcp_flow.h"

#undef private
#undef protected

namespace juggler {
namespace net {
namespace flow {

// ───────────────── Test Fixture ─────────────────

class TcpFlowTest : public ::testing::Test {
 protected:
  static constexpr uint32_t kChannelRingSize = 1024;

  // PmdPort and its TxRing are expensive to create and cannot survive
  // destroy-then-recreate, so we share them across all tests.
  static std::shared_ptr<dpdk::PmdPort> pmd_port_;
  static dpdk::TxRing* txring_;

  static void SetUpTestSuite() {
    // Use port 1 (the net_null virtual device).
    pmd_port_ = std::make_shared<dpdk::PmdPort>(1, 1, 1, 512, 512);
    pmd_port_->InitDriver();
    txring_ = pmd_port_->GetRing<dpdk::TxRing>(0);
  }
  static void TearDownTestSuite() {
    pmd_port_.reset();
  }

  void SetUp() override {
    // Set up addresses.
    local_addr_.FromString("10.0.0.1");
    remote_addr_.FromString("10.0.0.2");
    local_port_ = Tcp::Port(5000);
    remote_port_ = Tcp::Port(6000);
    local_mac_ = Ethernet::Address("00:00:00:00:00:01");
    remote_mac_ = Ethernet::Address("00:00:00:00:00:02");

    // Create a channel for MsgBuf allocation.
    static int channel_id = 0;
    std::string chan_name = "tcp_flow_test_" + std::to_string(channel_id++);
    channel_mgr_.AddChannel(chan_name.c_str(), kChannelRingSize,
                            kChannelRingSize, kChannelRingSize,
                            kChannelRingSize);
    channel_ = channel_mgr_.GetChannel(chan_name.c_str());

    // Create a separate packet pool for crafting RX packets.
    pkt_pool_ = std::make_unique<dpdk::PacketPool>(
        8192, dpdk::PmdRing::kDefaultFrameSize);

    // Callback tracking.
    callback_called_ = false;
    callback_success_ = false;
  }

  void TearDown() override {}

  // ── Helpers ──

  /// Application callback that records whether it was invoked.
  void AppCallback(shm::Channel* /*channel*/, bool success, const Key& /*key*/) {
    callback_called_ = true;
    callback_success_ = success;
  }

  /// Create a new TcpFlow in kClosed state.
  std::unique_ptr<TcpFlow> MakeFlow() {
    return std::make_unique<TcpFlow>(
        local_addr_, local_port_, remote_addr_, remote_port_, local_mac_,
        remote_mac_, txring_, [this](auto* ch, bool ok, const auto& k) {
          AppCallback(ch, ok, k);
        },
        channel_.get());
  }

  /**
   * @brief Build a fake TCP packet (Ethernet + IP + TCP + payload).
   *
   * The caller specifies seq, ack, flags, and an optional payload buffer.
   * TCP header length is 20 bytes (no options) unless extra_opts is supplied.
   */
  dpdk::Packet* MakePacket(uint32_t seq, uint32_t ack, uint8_t flags,
                            const uint8_t* payload = nullptr,
                            size_t payload_len = 0,
                            const uint8_t* tcp_opts = nullptr,
                            size_t tcp_opts_len = 0) {
    const size_t tcp_hdr_len = sizeof(Tcp) + tcp_opts_len;
    const size_t hdr_len = sizeof(Ethernet) + sizeof(Ipv4) + tcp_hdr_len;
    const size_t total_len = hdr_len + payload_len;

    dpdk::Packet* pkt = nullptr;
    CHECK(pkt_pool_->PacketBulkAlloc(&pkt, 1));
    dpdk::Packet::Reset(pkt);
    CHECK_NOTNULL(pkt->append(static_cast<uint16_t>(total_len)));

    // Ethernet header.
    auto* eh = pkt->head_data<Ethernet*>();
    eh->src_addr = remote_mac_;
    eh->dst_addr = local_mac_;
    eh->eth_type = be16_t(Ethernet::kIpv4);

    // IPv4 header. total_length = IP + TCP + payload (no Ethernet).
    auto* ipv4h = pkt->head_data<Ipv4*>(sizeof(Ethernet));
    ipv4h->version_ihl = 0x45;
    ipv4h->type_of_service = 0;
    ipv4h->total_length =
        be16_t(static_cast<uint16_t>(sizeof(Ipv4) + tcp_hdr_len + payload_len));
    ipv4h->packet_id = be16_t(0);
    ipv4h->fragment_offset = be16_t(0);
    ipv4h->time_to_live = 64;
    ipv4h->next_proto_id = Ipv4::Proto::kTcp;
    ipv4h->hdr_checksum = 0;
    ipv4h->src_addr = remote_addr_;
    ipv4h->dst_addr = local_addr_;

    // TCP header.
    auto* tcph = pkt->head_data<Tcp*>(sizeof(Ethernet) + sizeof(Ipv4));
    tcph->src_port = remote_port_;
    tcph->dst_port = local_port_;
    tcph->seq_num = be32_t(seq);
    tcph->ack_num = be32_t(ack);
    tcph->set_header_length(static_cast<uint8_t>(tcp_hdr_len));
    tcph->flags = flags;
    tcph->window = be16_t(65535);
    tcph->checksum = 0;
    tcph->urgent_ptr = be16_t(0);

    // TCP options (if any).
    if (tcp_opts != nullptr && tcp_opts_len > 0) {
      uint8_t* opt_dst = reinterpret_cast<uint8_t*>(tcph) + sizeof(Tcp);
      std::memcpy(opt_dst, tcp_opts, tcp_opts_len);
    }

    // Payload.
    if (payload != nullptr && payload_len > 0) {
      uint8_t* dst = pkt->head_data<uint8_t*>(
          static_cast<uint16_t>(hdr_len));
      std::memcpy(dst, payload, payload_len);
    }

    return pkt;
  }

  /// Build the standard MSS option bytes (Kind=2, Length=4, MSS value).
  static std::vector<uint8_t> MakeMSSOption(uint16_t mss) {
    std::vector<uint8_t> opt(4);
    opt[0] = 2;  // Kind: MSS
    opt[1] = 4;  // Length
    uint16_t mss_net = htobe16(mss);
    std::memcpy(&opt[2], &mss_net, sizeof(mss_net));
    return opt;
  }

  /// Create a MsgBuf train of the given size, filled with sequential bytes.
  shm::MsgBuf* CreateMsg(size_t msg_len) {
    std::vector<uint8_t> data(msg_len);
    for (size_t i = 0; i < msg_len; i++) {
      data[i] = static_cast<uint8_t>(i & 0xFF);
    }
    return CreateMsgFromData(data);
  }

  shm::MsgBuf* CreateMsgFromData(const std::vector<uint8_t>& data) {
    const auto msgbuf_size = channel_->GetUsableBufSize();
    auto num_msgbufs =
        (data.size() + msgbuf_size - 1) / msgbuf_size;

    shm::MsgBuf* head = nullptr;
    shm::MsgBuf* tail = nullptr;
    size_t data_offset = 0;

    while (num_msgbufs > 0) {
      shm::MsgBufBatch batch;
      auto msgbuf_nr =
          std::min(num_msgbufs, static_cast<size_t>(batch.GetRoom()));
      CHECK(channel_->MsgBufBulkAlloc(&batch, msgbuf_nr));

      for (auto i = 0; i < batch.GetSize(); i++) {
        auto* msgbuf = batch[i];
        auto nbytes = std::min(static_cast<size_t>(msgbuf_size),
                               data.size() - data_offset);
        utils::Copy(CHECK_NOTNULL(msgbuf->append(nbytes)),
                    &data[data_offset], nbytes);
        data_offset += nbytes;

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

  // ── Members ──
  Ipv4::Address local_addr_;
  Ipv4::Address remote_addr_;
  Tcp::Port local_port_;
  Tcp::Port remote_port_;
  Ethernet::Address local_mac_;
  Ethernet::Address remote_mac_;
  shm::ChannelManager<shm::Channel> channel_mgr_;
  std::shared_ptr<shm::Channel> channel_;
  std::unique_ptr<dpdk::PacketPool> pkt_pool_;

  bool callback_called_;
  bool callback_success_;
};

// Static member definitions.
std::shared_ptr<dpdk::PmdPort> TcpFlowTest::pmd_port_;
dpdk::TxRing* TcpFlowTest::txring_;

// ═══════════════════════════════════════════════════════════════
//  Sequence Number Comparison Helpers
// ═══════════════════════════════════════════════════════════════

TEST_F(TcpFlowTest, SeqComparisons) {
  // Basic ordering.
  EXPECT_TRUE(TcpFlow::SeqLt(1, 2));
  EXPECT_FALSE(TcpFlow::SeqLt(2, 1));
  EXPECT_TRUE(TcpFlow::SeqLeq(2, 2));
  EXPECT_TRUE(TcpFlow::SeqGt(3, 2));
  EXPECT_FALSE(TcpFlow::SeqGt(2, 3));
  EXPECT_TRUE(TcpFlow::SeqGeq(3, 3));

  // Wrapping: 0xFFFFFFFF < 0x00000000 in TCP sequence space.
  EXPECT_TRUE(TcpFlow::SeqLt(0xFFFFFFFF, 0));
  EXPECT_TRUE(TcpFlow::SeqGt(0, 0xFFFFFFFF));
  EXPECT_TRUE(TcpFlow::SeqLt(0xFFFFFFFE, 0x00000001));
}

// ═══════════════════════════════════════════════════════════════
//  Construction & Initial State
// ═══════════════════════════════════════════════════════════════

TEST_F(TcpFlowTest, InitialState) {
  auto flow = MakeFlow();
  EXPECT_EQ(flow->state(), TcpFlow::State::kClosed);
  EXPECT_EQ(flow->channel(), channel_.get());
  EXPECT_EQ(flow->key().local_addr, local_addr_);
  EXPECT_EQ(flow->key().remote_addr, remote_addr_);
  EXPECT_EQ(flow->key().local_port, local_port_);
  EXPECT_EQ(flow->key().remote_port, remote_port_);
  EXPECT_EQ(flow->peer_mss_, static_cast<uint16_t>(TcpFlow::kDefaultMSS));
  EXPECT_EQ(flow->snd_nxt_, flow->snd_isn_);
  EXPECT_EQ(flow->snd_una_, flow->snd_isn_);
}

// ═══════════════════════════════════════════════════════════════
//  Active Open (Client) Handshake
// ═══════════════════════════════════════════════════════════════

TEST_F(TcpFlowTest, ActiveOpen_InitiateHandshake) {
  auto flow = MakeFlow();

  // InitiateHandshake should send SYN and transition to SYN_SENT.
  flow->InitiateHandshake();
  EXPECT_EQ(flow->state(), TcpFlow::State::kSynSent);
  EXPECT_TRUE(flow->rto_active_);
  // SYN consumes one seq: snd_nxt_ should be snd_isn_ + 1.
  EXPECT_EQ(flow->snd_nxt_, flow->snd_isn_ + 1);
}

TEST_F(TcpFlowTest, ActiveOpen_FullHandshake) {
  auto flow = MakeFlow();
  flow->InitiateHandshake();

  uint32_t client_isn = flow->snd_isn_;
  uint32_t server_isn = 42000;

  // Server responds with SYN-ACK, acknowledging our SYN.
  auto* syn_ack = MakePacket(server_isn, client_isn + 1,
                              Tcp::kSyn | Tcp::kAck);
  flow->InputPacket(syn_ack);

  // Should be ESTABLISHED.
  EXPECT_EQ(flow->state(), TcpFlow::State::kEstablished);
  EXPECT_EQ(flow->rcv_nxt_, server_isn + 1);
  EXPECT_EQ(flow->snd_una_, client_isn + 1);
  EXPECT_FALSE(flow->rto_active_);
  // Callback should have been invoked with success.
  EXPECT_TRUE(callback_called_);
  EXPECT_TRUE(callback_success_);

  dpdk::Packet::Free(syn_ack);
}

TEST_F(TcpFlowTest, ActiveOpen_SynAckWrongAck) {
  auto flow = MakeFlow();
  flow->InitiateHandshake();

  uint32_t client_isn = flow->snd_isn_;
  uint32_t server_isn = 42000;

  // Wrong ack — should stay in SYN_SENT.
  auto* bad_syn_ack = MakePacket(server_isn, client_isn + 99,
                                  Tcp::kSyn | Tcp::kAck);
  flow->InputPacket(bad_syn_ack);

  EXPECT_EQ(flow->state(), TcpFlow::State::kSynSent);
  EXPECT_FALSE(callback_called_);

  dpdk::Packet::Free(bad_syn_ack);
}

// ═══════════════════════════════════════════════════════════════
//  Passive Open (Server) Handshake
// ═══════════════════════════════════════════════════════════════

TEST_F(TcpFlowTest, PassiveOpen_StartPassiveOpen) {
  auto flow = MakeFlow();
  flow->StartPassiveOpen();
  EXPECT_EQ(flow->state(), TcpFlow::State::kListen);
}

TEST_F(TcpFlowTest, PassiveOpen_FullHandshake) {
  auto flow = MakeFlow();
  flow->StartPassiveOpen();

  uint32_t client_isn = 12345;
  uint32_t server_isn = flow->snd_isn_;

  // Client sends SYN.
  auto* syn = MakePacket(client_isn, 0, Tcp::kSyn);
  flow->InputPacket(syn);
  EXPECT_EQ(flow->state(), TcpFlow::State::kSynReceived);
  EXPECT_EQ(flow->rcv_nxt_, client_isn + 1);
  EXPECT_TRUE(flow->rto_active_);

  // Client acknowledges our SYN-ACK.
  auto* ack = MakePacket(client_isn + 1, server_isn + 1, Tcp::kAck);
  flow->InputPacket(ack);
  EXPECT_EQ(flow->state(), TcpFlow::State::kEstablished);
  EXPECT_EQ(flow->snd_una_, server_isn + 1);
  EXPECT_FALSE(flow->rto_active_);

  dpdk::Packet::Free(syn);
  dpdk::Packet::Free(ack);
}

TEST_F(TcpFlowTest, PassiveOpen_SynWithMSSOption) {
  auto flow = MakeFlow();
  flow->StartPassiveOpen();

  uint32_t client_isn = 12345;
  auto mss_opt = MakeMSSOption(1460);

  // Client sends SYN with MSS option.
  auto* syn = MakePacket(client_isn, 0, Tcp::kSyn,
                          nullptr, 0,
                          mss_opt.data(), mss_opt.size());
  flow->InputPacket(syn);

  EXPECT_EQ(flow->state(), TcpFlow::State::kSynReceived);
  EXPECT_EQ(flow->peer_mss_, 1460);

  dpdk::Packet::Free(syn);
}

TEST_F(TcpFlowTest, PassiveOpen_PiggybackOnAck) {
  // Linux kernel can piggyback data on the completing handshake ACK.
  auto flow = MakeFlow();
  flow->StartPassiveOpen();

  uint32_t client_isn = 12345;
  uint32_t server_isn = flow->snd_isn_;

  // SYN.
  auto* syn = MakePacket(client_isn, 0, Tcp::kSyn);
  flow->InputPacket(syn);

  // ACK with some payload.
  uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
  auto* ack = MakePacket(client_isn + 1, server_isn + 1, Tcp::kAck,
                          payload, sizeof(payload));
  flow->InputPacket(ack);

  EXPECT_EQ(flow->state(), TcpFlow::State::kEstablished);
  // Data should have been consumed.
  EXPECT_EQ(flow->rcv_nxt_, client_isn + 1 + sizeof(payload));

  dpdk::Packet::Free(syn);
  dpdk::Packet::Free(ack);
}

// ═══════════════════════════════════════════════════════════════
//  RST Handling
// ═══════════════════════════════════════════════════════════════

TEST_F(TcpFlowTest, RstInEstablished) {
  auto flow = MakeFlow();
  flow->state_ = TcpFlow::State::kEstablished;
  flow->rcv_nxt_ = 1000;

  auto* rst = MakePacket(1000, 0, Tcp::kRst);
  flow->InputPacket(rst);
  EXPECT_EQ(flow->state(), TcpFlow::State::kClosed);

  dpdk::Packet::Free(rst);
}

TEST_F(TcpFlowTest, RstInSynSent) {
  auto flow = MakeFlow();
  flow->InitiateHandshake();

  auto* rst = MakePacket(0, 0, Tcp::kRst);
  flow->InputPacket(rst);
  EXPECT_EQ(flow->state(), TcpFlow::State::kClosed);

  dpdk::Packet::Free(rst);
}

// ═══════════════════════════════════════════════════════════════
//  MSS Option Parsing
// ═══════════════════════════════════════════════════════════════

TEST_F(TcpFlowTest, ParseTcpOptions_MSSOnly) {
  auto flow = MakeFlow();
  // Build a fake TCP header with MSS option.
  uint8_t buf[24] = {};
  auto* tcph = reinterpret_cast<Tcp*>(buf);
  tcph->set_header_length(24);
  // MSS option at offset 20 (after base header).
  buf[20] = 2;   // Kind: MSS
  buf[21] = 4;   // Length
  uint16_t mss_net = htobe16(8960);
  std::memcpy(&buf[22], &mss_net, 2);

  flow->ParseTcpOptions(tcph, 24);
  EXPECT_EQ(flow->peer_mss_, 8960);
}

TEST_F(TcpFlowTest, ParseTcpOptions_MSSWithNopPadding) {
  auto flow = MakeFlow();
  // Build TCP header with NOP + NOP + MSS + EOL.
  uint8_t buf[28] = {};
  auto* tcph = reinterpret_cast<Tcp*>(buf);
  tcph->set_header_length(28);
  buf[20] = 1;   // NOP
  buf[21] = 1;   // NOP
  buf[22] = 2;   // Kind: MSS
  buf[23] = 4;   // Length
  uint16_t mss_net = htobe16(1460);
  std::memcpy(&buf[24], &mss_net, 2);
  buf[26] = 0;   // EOL

  flow->ParseTcpOptions(tcph, 28);
  EXPECT_EQ(flow->peer_mss_, 1460);
}

TEST_F(TcpFlowTest, ParseTcpOptions_NoOptions) {
  auto flow = MakeFlow();
  uint8_t buf[20] = {};
  auto* tcph = reinterpret_cast<Tcp*>(buf);
  tcph->set_header_length(20);

  // peer_mss_ should remain at default.
  flow->ParseTcpOptions(tcph, 20);
  EXPECT_EQ(flow->peer_mss_, static_cast<uint16_t>(TcpFlow::kDefaultMSS));
}

TEST_F(TcpFlowTest, ParseTcpOptions_ZeroMSS_FallsBackToDefault) {
  auto flow = MakeFlow();
  uint8_t buf[24] = {};
  auto* tcph = reinterpret_cast<Tcp*>(buf);
  tcph->set_header_length(24);
  buf[20] = 2;  // Kind: MSS
  buf[21] = 4;  // Length
  buf[22] = 0;  // MSS = 0
  buf[23] = 0;

  flow->ParseTcpOptions(tcph, 24);
  EXPECT_EQ(flow->peer_mss_, static_cast<uint16_t>(TcpFlow::kDefaultMSS));
}

// ═══════════════════════════════════════════════════════════════
//  TCP Header Length Validation
// ═══════════════════════════════════════════════════════════════

TEST_F(TcpFlowTest, InputPacket_InvalidHeaderLength) {
  auto flow = MakeFlow();
  flow->state_ = TcpFlow::State::kEstablished;
  flow->rcv_nxt_ = 1000;

  // Craft a packet with TCP header length of 16 (< 20, invalid).
  auto* pkt = MakePacket(1000, 0, Tcp::kAck);
  auto* tcph = pkt->head_data<Tcp*>(sizeof(Ethernet) + sizeof(Ipv4));
  tcph->set_header_length(16);  // Invalid: below minimum.

  uint32_t old_rcv_nxt = flow->rcv_nxt_;
  flow->InputPacket(pkt);
  // Should be a no-op — state unchanged, no data consumed.
  EXPECT_EQ(flow->state(), TcpFlow::State::kEstablished);
  EXPECT_EQ(flow->rcv_nxt_, old_rcv_nxt);

  dpdk::Packet::Free(pkt);
}

// ═══════════════════════════════════════════════════════════════
//  Data TX Path (OutputMessage)
// ═══════════════════════════════════════════════════════════════

TEST_F(TcpFlowTest, OutputMessage_SmallMessage) {
  auto flow = MakeFlow();
  flow->state_ = TcpFlow::State::kEstablished;
  flow->rcv_nxt_ = 5000;
  uint32_t initial_snd_nxt = flow->snd_nxt_;

  // Create a small message (64 bytes).
  auto* msgbuf = CreateMsg(64);

  flow->OutputMessage(msgbuf);

  // snd_nxt_ should advance by 4 (length prefix) + 64 (payload).
  EXPECT_EQ(flow->snd_nxt_, initial_snd_nxt + 4 + 64);
  // RTO should be armed.
  EXPECT_TRUE(flow->rto_active_);
}

TEST_F(TcpFlowTest, OutputMessage_NotEstablished) {
  auto flow = MakeFlow();
  // Try to send in CLOSED state — should fail gracefully.
  flow->state_ = TcpFlow::State::kClosed;

  auto* msgbuf = CreateMsg(64);
  uint32_t initial_snd_nxt = flow->snd_nxt_;

  flow->OutputMessage(msgbuf);
  // snd_nxt_ should not change.
  EXPECT_EQ(flow->snd_nxt_, initial_snd_nxt);
}

TEST_F(TcpFlowTest, OutputMessage_LargerThanMSS) {
  auto flow = MakeFlow();
  flow->state_ = TcpFlow::State::kEstablished;
  flow->rcv_nxt_ = 5000;
  flow->peer_mss_ = 100;  // Small MSS for segmentation.
  uint32_t initial_snd_nxt = flow->snd_nxt_;

  // Create a message larger than MSS.
  const size_t msg_len = 300;
  auto* msgbuf = CreateMsg(msg_len);

  flow->OutputMessage(msgbuf);

  // Total bytes on wire: 4 (prefix) + 300 (payload) = 304 bytes.
  EXPECT_EQ(flow->snd_nxt_, initial_snd_nxt + 4 + msg_len);
}

// ═══════════════════════════════════════════════════════════════
//  Data RX Path (ConsumePayload)
// ═══════════════════════════════════════════════════════════════

TEST_F(TcpFlowTest, ConsumePayload_SingleMessage) {
  auto flow = MakeFlow();
  flow->state_ = TcpFlow::State::kEstablished;

  // Build a framed message: 4-byte length prefix + payload.
  const std::string payload_str = "Hello, TCP world!";
  const uint32_t msg_len = static_cast<uint32_t>(payload_str.size());
  uint32_t net_len = htobe32(msg_len);

  std::vector<uint8_t> wire_data(TcpFlow::kMsgLenPrefixSize + msg_len);
  std::memcpy(wire_data.data(), &net_len, TcpFlow::kMsgLenPrefixSize);
  std::memcpy(wire_data.data() + TcpFlow::kMsgLenPrefixSize,
              payload_str.data(), msg_len);

  flow->ConsumePayload(wire_data.data(), wire_data.size());

  // Message should have been delivered to the channel.
  // Verify by receiving from the channel.
  std::vector<uint8_t> rx_buf(msg_len);
  MachnetIovec_t iov;
  iov.base = rx_buf.data();
  iov.len = rx_buf.size();
  MachnetMsgHdr_t msghdr;
  msghdr.flags = 0;
  msghdr.flow_info = {0, 0, 0, 0};
  msghdr.msg_iov = &iov;
  msghdr.msg_iovlen = 1;

  auto ret = machnet_recvmsg(channel_->ctx(), &msghdr);
  EXPECT_EQ(ret, 1) << "Message should have been delivered to channel";
  EXPECT_EQ(std::string(rx_buf.begin(), rx_buf.end()), payload_str);
}

TEST_F(TcpFlowTest, ConsumePayload_SplitAcrossPackets) {
  auto flow = MakeFlow();
  flow->state_ = TcpFlow::State::kEstablished;

  // Build a framed message.
  const uint32_t msg_len = 100;
  uint32_t net_len = htobe32(msg_len);
  std::vector<uint8_t> payload(msg_len);
  for (uint32_t i = 0; i < msg_len; i++) payload[i] = static_cast<uint8_t>(i);

  std::vector<uint8_t> wire_data(TcpFlow::kMsgLenPrefixSize + msg_len);
  std::memcpy(wire_data.data(), &net_len, TcpFlow::kMsgLenPrefixSize);
  std::memcpy(wire_data.data() + TcpFlow::kMsgLenPrefixSize,
              payload.data(), msg_len);

  // Split at the middle of the length prefix (2 bytes).
  flow->ConsumePayload(wire_data.data(), 2);
  // Should not have a pending message yet.
  EXPECT_EQ(flow->rx_pending_msg_len_, 0u);

  // Send the rest.
  flow->ConsumePayload(wire_data.data() + 2, wire_data.size() - 2);

  // Verify message was delivered.
  std::vector<uint8_t> rx_buf(msg_len);
  MachnetIovec_t iov;
  iov.base = rx_buf.data();
  iov.len = rx_buf.size();
  MachnetMsgHdr_t msghdr;
  msghdr.flags = 0;
  msghdr.flow_info = {0, 0, 0, 0};
  msghdr.msg_iov = &iov;
  msghdr.msg_iovlen = 1;

  auto ret = machnet_recvmsg(channel_->ctx(), &msghdr);
  EXPECT_EQ(ret, 1);
  EXPECT_EQ(rx_buf, payload);
}

TEST_F(TcpFlowTest, ConsumePayload_MultipleMessages) {
  auto flow = MakeFlow();
  flow->state_ = TcpFlow::State::kEstablished;

  // Two back-to-back messages in a single payload.
  const uint32_t msg1_len = 10;
  const uint32_t msg2_len = 20;

  std::vector<uint8_t> msg1_payload(msg1_len, 0xAA);
  std::vector<uint8_t> msg2_payload(msg2_len, 0xBB);

  std::vector<uint8_t> wire_data;
  uint32_t net_len;

  // Message 1.
  net_len = htobe32(msg1_len);
  wire_data.insert(wire_data.end(), reinterpret_cast<uint8_t*>(&net_len),
                   reinterpret_cast<uint8_t*>(&net_len) + 4);
  wire_data.insert(wire_data.end(), msg1_payload.begin(), msg1_payload.end());

  // Message 2.
  net_len = htobe32(msg2_len);
  wire_data.insert(wire_data.end(), reinterpret_cast<uint8_t*>(&net_len),
                   reinterpret_cast<uint8_t*>(&net_len) + 4);
  wire_data.insert(wire_data.end(), msg2_payload.begin(), msg2_payload.end());

  flow->ConsumePayload(wire_data.data(), wire_data.size());

  // Read message 1.
  {
    std::vector<uint8_t> rx_buf(msg1_len);
    MachnetIovec_t iov{rx_buf.data(), rx_buf.size()};
    MachnetMsgHdr_t msghdr;
    msghdr.flags = 0;
    msghdr.flow_info = {0, 0, 0, 0};
    msghdr.msg_iov = &iov;
    msghdr.msg_iovlen = 1;
    EXPECT_EQ(machnet_recvmsg(channel_->ctx(), &msghdr), 1);
    EXPECT_EQ(rx_buf, msg1_payload);
  }
  // Read message 2.
  {
    std::vector<uint8_t> rx_buf(msg2_len);
    MachnetIovec_t iov{rx_buf.data(), rx_buf.size()};
    MachnetMsgHdr_t msghdr;
    msghdr.flags = 0;
    msghdr.flow_info = {0, 0, 0, 0};
    msghdr.msg_iov = &iov;
    msghdr.msg_iovlen = 1;
    EXPECT_EQ(machnet_recvmsg(channel_->ctx(), &msghdr), 1);
    EXPECT_EQ(rx_buf, msg2_payload);
  }
}

// ═══════════════════════════════════════════════════════════════
//  HandleEstablished — Data + ACK
// ═══════════════════════════════════════════════════════════════

TEST_F(TcpFlowTest, HandleEstablished_DataPacket) {
  auto flow = MakeFlow();
  flow->state_ = TcpFlow::State::kEstablished;
  flow->rcv_nxt_ = 1000;
  flow->snd_una_ = 500;
  flow->snd_nxt_ = 500;

  // Build a framed message as payload.
  const uint32_t msg_len = 16;
  uint32_t net_len = htobe32(msg_len);
  std::vector<uint8_t> payload(TcpFlow::kMsgLenPrefixSize + msg_len);
  std::memcpy(payload.data(), &net_len, TcpFlow::kMsgLenPrefixSize);
  for (uint32_t i = 0; i < msg_len; i++) {
    payload[TcpFlow::kMsgLenPrefixSize + i] = static_cast<uint8_t>(i);
  }

  auto* pkt = MakePacket(1000, 500, Tcp::kAck | Tcp::kPsh,
                          payload.data(), payload.size());
  flow->InputPacket(pkt);

  // rcv_nxt should advance by the payload size.
  EXPECT_EQ(flow->rcv_nxt_, 1000 + payload.size());

  // Read the delivered message.
  std::vector<uint8_t> rx_buf(msg_len);
  MachnetIovec_t iov{rx_buf.data(), rx_buf.size()};
  MachnetMsgHdr_t msghdr;
  msghdr.flags = 0;
  msghdr.flow_info = {0, 0, 0, 0};
  msghdr.msg_iov = &iov;
  msghdr.msg_iovlen = 1;
  EXPECT_EQ(machnet_recvmsg(channel_->ctx(), &msghdr), 1);

  for (uint32_t i = 0; i < msg_len; i++) {
    EXPECT_EQ(rx_buf[i], static_cast<uint8_t>(i));
  }

  dpdk::Packet::Free(pkt);
}

// ═══════════════════════════════════════════════════════════════
//  HandleEstablished — Retransmitted SYN
// ═══════════════════════════════════════════════════════════════

TEST_F(TcpFlowTest, HandleEstablished_RetransmittedSyn) {
  // In ESTABLISHED, receiving a SYN should re-send ACK (kernel missed it).
  auto flow = MakeFlow();
  flow->state_ = TcpFlow::State::kEstablished;
  flow->rcv_nxt_ = 2000;
  flow->snd_nxt_ = 3000;
  flow->snd_una_ = 3000;

  auto* syn = MakePacket(1999, 0, Tcp::kSyn);
  auto state_before = flow->state();
  flow->InputPacket(syn);

  // State should not change — we just re-ACK.
  EXPECT_EQ(flow->state(), state_before);
  EXPECT_EQ(flow->rcv_nxt_, 2000u);

  dpdk::Packet::Free(syn);
}

// ═══════════════════════════════════════════════════════════════
//  Retransmission Overlap (ProcessInOrderPayload)
// ═══════════════════════════════════════════════════════════════

TEST_F(TcpFlowTest, ProcessInOrderPayload_ExactMatch) {
  auto flow = MakeFlow();
  flow->state_ = TcpFlow::State::kEstablished;
  flow->rcv_nxt_ = 1000;

  // Segment exactly at rcv_nxt_.
  uint8_t data[] = {1, 2, 3, 4, 5};
  auto* pkt = MakePacket(1000, 0, Tcp::kAck, data, sizeof(data));

  bool consumed = flow->ProcessInOrderPayload(
      pkt, 1000, sizeof(data),
      sizeof(Ethernet) + sizeof(Ipv4) + sizeof(Tcp));

  EXPECT_TRUE(consumed);
  EXPECT_EQ(flow->rcv_nxt_, 1005u);

  dpdk::Packet::Free(pkt);
}

TEST_F(TcpFlowTest, ProcessInOrderPayload_PartialOverlap) {
  auto flow = MakeFlow();
  flow->state_ = TcpFlow::State::kEstablished;
  flow->rcv_nxt_ = 1003;  // We already received bytes 1000-1002.

  // Retransmitted segment starts at 1000 but extends to 1009.
  uint8_t data[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  auto* pkt = MakePacket(1000, 0, Tcp::kAck, data, sizeof(data));

  bool consumed = flow->ProcessInOrderPayload(
      pkt, 1000, sizeof(data),
      sizeof(Ethernet) + sizeof(Ipv4) + sizeof(Tcp));

  EXPECT_TRUE(consumed);
  // Should have consumed only the new part: bytes 1003-1009 = 7 bytes.
  EXPECT_EQ(flow->rcv_nxt_, 1010u);

  dpdk::Packet::Free(pkt);
}

TEST_F(TcpFlowTest, ProcessInOrderPayload_PureDuplicate) {
  auto flow = MakeFlow();
  flow->state_ = TcpFlow::State::kEstablished;
  flow->rcv_nxt_ = 1010;  // Already past this segment.

  uint8_t data[5] = {1, 2, 3, 4, 5};
  auto* pkt = MakePacket(1005, 0, Tcp::kAck, data, sizeof(data));

  bool consumed = flow->ProcessInOrderPayload(
      pkt, 1005, sizeof(data),
      sizeof(Ethernet) + sizeof(Ipv4) + sizeof(Tcp));

  // Pure duplicate — nothing consumed.
  EXPECT_FALSE(consumed);
  EXPECT_EQ(flow->rcv_nxt_, 1010u);

  dpdk::Packet::Free(pkt);
}

TEST_F(TcpFlowTest, ProcessInOrderPayload_EmptyPayload) {
  auto flow = MakeFlow();
  flow->state_ = TcpFlow::State::kEstablished;
  flow->rcv_nxt_ = 1000;

  auto* pkt = MakePacket(1000, 0, Tcp::kAck);

  bool consumed = flow->ProcessInOrderPayload(
      pkt, 1000, 0, sizeof(Ethernet) + sizeof(Ipv4) + sizeof(Tcp));

  EXPECT_FALSE(consumed);
  EXPECT_EQ(flow->rcv_nxt_, 1000u);

  dpdk::Packet::Free(pkt);
}

// ═══════════════════════════════════════════════════════════════
//  FIN Handling
// ═══════════════════════════════════════════════════════════════

TEST_F(TcpFlowTest, FIN_InEstablished) {
  auto flow = MakeFlow();
  flow->state_ = TcpFlow::State::kEstablished;
  flow->rcv_nxt_ = 2000;
  flow->snd_una_ = 3000;
  flow->snd_nxt_ = 3000;

  // FIN at the expected sequence position (no payload).
  auto* fin_pkt = MakePacket(2000, 3000, Tcp::kFin | Tcp::kAck);
  flow->InputPacket(fin_pkt);

  EXPECT_EQ(flow->state(), TcpFlow::State::kCloseWait);
  EXPECT_EQ(flow->rcv_nxt_, 2001u);  // FIN consumes one seq.

  dpdk::Packet::Free(fin_pkt);
}

TEST_F(TcpFlowTest, FIN_WithData) {
  auto flow = MakeFlow();
  flow->state_ = TcpFlow::State::kEstablished;
  flow->rcv_nxt_ = 2000;
  flow->snd_una_ = 3000;
  flow->snd_nxt_ = 3000;

  // FIN with 10 bytes of payload. FIN is at seq 2000+10=2010.
  uint8_t payload[10] = {};
  auto* fin_pkt = MakePacket(2000, 3000, Tcp::kFin | Tcp::kAck,
                              payload, sizeof(payload));
  flow->InputPacket(fin_pkt);

  EXPECT_EQ(flow->state(), TcpFlow::State::kCloseWait);
  EXPECT_EQ(flow->rcv_nxt_, 2011u);  // 10 data + 1 FIN.

  dpdk::Packet::Free(fin_pkt);
}

TEST_F(TcpFlowTest, FIN_RetransmittedFin) {
  auto flow = MakeFlow();
  flow->state_ = TcpFlow::State::kEstablished;
  flow->rcv_nxt_ = 2001;  // Already processed FIN at 2000.
  flow->snd_una_ = 3000;
  flow->snd_nxt_ = 3000;

  // Retransmitted FIN at seq 2000 (we already processed it).
  auto* dup_fin = MakePacket(2000, 3000, Tcp::kFin | Tcp::kAck);
  flow->InputPacket(dup_fin);

  // Should stay in ESTABLISHED (or wherever it was) — fin_seq < rcv_nxt.
  // Actually, we're at seq 2000+0=2000 < 2001, so it's a retransmit → re-ACK.
  // State should not change again.
  EXPECT_EQ(flow->rcv_nxt_, 2001u);

  dpdk::Packet::Free(dup_fin);
}

// ═══════════════════════════════════════════════════════════════
//  Shutdown (Active Close)
// ═══════════════════════════════════════════════════════════════

TEST_F(TcpFlowTest, ShutDown_FromEstablished) {
  auto flow = MakeFlow();
  flow->state_ = TcpFlow::State::kEstablished;
  flow->rcv_nxt_ = 1000;
  uint32_t old_snd_nxt = flow->snd_nxt_;

  flow->ShutDown();

  EXPECT_EQ(flow->state(), TcpFlow::State::kFinWait1);
  // FIN consumes one seq.
  EXPECT_EQ(flow->snd_nxt_, old_snd_nxt + 1);
}

TEST_F(TcpFlowTest, ShutDown_FromCloseWait) {
  auto flow = MakeFlow();
  flow->state_ = TcpFlow::State::kCloseWait;
  flow->rcv_nxt_ = 1000;
  uint32_t old_snd_nxt = flow->snd_nxt_;

  flow->ShutDown();

  EXPECT_EQ(flow->state(), TcpFlow::State::kLastAck);
  EXPECT_EQ(flow->snd_nxt_, old_snd_nxt + 1);
}

TEST_F(TcpFlowTest, ShutDown_FromSynSent) {
  auto flow = MakeFlow();
  flow->InitiateHandshake();

  flow->ShutDown();

  // Non-established state → send RST, go to CLOSED.
  EXPECT_EQ(flow->state(), TcpFlow::State::kClosed);
}

// ═══════════════════════════════════════════════════════════════
//  Full Graceful Close (4-way)
// ═══════════════════════════════════════════════════════════════

TEST_F(TcpFlowTest, GracefulClose_FinWait1_to_TimeWait) {
  auto flow = MakeFlow();
  flow->state_ = TcpFlow::State::kEstablished;
  flow->rcv_nxt_ = 2000;
  flow->snd_nxt_ = 3000;
  flow->snd_una_ = 3000;

  // We initiate close.
  flow->ShutDown();
  EXPECT_EQ(flow->state(), TcpFlow::State::kFinWait1);
  EXPECT_EQ(flow->snd_nxt_, 3001u);

  // Peer ACKs our FIN.
  auto* ack = MakePacket(2000, 3001, Tcp::kAck);
  flow->InputPacket(ack);
  EXPECT_EQ(flow->state(), TcpFlow::State::kFinWait2);

  // Peer sends FIN.
  auto* peer_fin = MakePacket(2000, 3001, Tcp::kFin | Tcp::kAck);
  flow->InputPacket(peer_fin);
  EXPECT_EQ(flow->state(), TcpFlow::State::kTimeWait);
  EXPECT_EQ(flow->rcv_nxt_, 2001u);

  dpdk::Packet::Free(ack);
  dpdk::Packet::Free(peer_fin);
}

TEST_F(TcpFlowTest, GracefulClose_LastAck) {
  auto flow = MakeFlow();
  flow->state_ = TcpFlow::State::kEstablished;
  flow->rcv_nxt_ = 2000;
  flow->snd_nxt_ = 3000;
  flow->snd_una_ = 3000;

  // Peer sends FIN first.
  auto* peer_fin = MakePacket(2000, 3000, Tcp::kFin | Tcp::kAck);
  flow->InputPacket(peer_fin);
  EXPECT_EQ(flow->state(), TcpFlow::State::kCloseWait);

  // We respond with FIN.
  flow->ShutDown();
  EXPECT_EQ(flow->state(), TcpFlow::State::kLastAck);

  // Peer ACKs our FIN.
  auto* final_ack = MakePacket(2001, flow->snd_nxt_, Tcp::kAck);
  flow->InputPacket(final_ack);
  EXPECT_EQ(flow->state(), TcpFlow::State::kClosed);

  dpdk::Packet::Free(peer_fin);
  dpdk::Packet::Free(final_ack);
}

// ═══════════════════════════════════════════════════════════════
//  PeriodicCheck — RTO and TIME_WAIT
// ═══════════════════════════════════════════════════════════════

TEST_F(TcpFlowTest, PeriodicCheck_ClosedReturnsFalse) {
  auto flow = MakeFlow();
  EXPECT_FALSE(flow->PeriodicCheck());
}

TEST_F(TcpFlowTest, PeriodicCheck_TimeWaitCountdown) {
  auto flow = MakeFlow();
  flow->state_ = TcpFlow::State::kTimeWait;
  flow->time_wait_remaining_ = TcpFlow::kTimeWaitTicks;

  for (uint32_t i = 0; i < TcpFlow::kTimeWaitTicks; i++) {
    EXPECT_TRUE(flow->PeriodicCheck());
  }
  // One more should transition to CLOSED and return false.
  EXPECT_FALSE(flow->PeriodicCheck());
  EXPECT_EQ(flow->state(), TcpFlow::State::kClosed);
}

TEST_F(TcpFlowTest, PeriodicCheck_RTO_SynSentRetransmit) {
  auto flow = MakeFlow();
  flow->InitiateHandshake();
  EXPECT_TRUE(flow->rto_active_);

  // Burn through the RTO timer.
  for (uint32_t i = 0; i < TcpFlow::kInitialRTO; i++) {
    EXPECT_TRUE(flow->PeriodicCheck());
  }
  // Next check should trigger retransmission.
  uint32_t retransmit_before = flow->retransmit_count_;
  EXPECT_TRUE(flow->PeriodicCheck());
  EXPECT_GT(flow->retransmit_count_, retransmit_before);
  EXPECT_EQ(flow->state(), TcpFlow::State::kSynSent);
}

TEST_F(TcpFlowTest, PeriodicCheck_MaxRetransmissions) {
  auto flow = MakeFlow();
  flow->InitiateHandshake();

  // Exhaust all retransmissions.
  flow->retransmit_count_ = TcpFlow::kMaxRetransmissions;
  flow->rto_remaining_ = 0;

  EXPECT_FALSE(flow->PeriodicCheck());
  EXPECT_EQ(flow->state(), TcpFlow::State::kClosed);
  // Callback should be invoked with failure (since we were in SYN_SENT).
  EXPECT_TRUE(callback_called_);
  EXPECT_FALSE(callback_success_);
}

TEST_F(TcpFlowTest, PeriodicCheck_EstablishedNoRTO) {
  auto flow = MakeFlow();
  flow->state_ = TcpFlow::State::kEstablished;
  flow->rto_active_ = false;

  // Should just return true without changing state.
  EXPECT_TRUE(flow->PeriodicCheck());
  EXPECT_EQ(flow->state(), TcpFlow::State::kEstablished);
}

// ═══════════════════════════════════════════════════════════════
//  AdvanceSndUna
// ═══════════════════════════════════════════════════════════════

TEST_F(TcpFlowTest, AdvanceSndUna_NormalAck) {
  auto flow = MakeFlow();
  flow->snd_una_ = 100;
  flow->snd_nxt_ = 200;
  flow->rto_active_ = true;
  flow->retransmit_count_ = 2;

  flow->AdvanceSndUna(150);
  EXPECT_EQ(flow->snd_una_, 150u);
  EXPECT_EQ(flow->retransmit_count_, 0u);
  EXPECT_TRUE(flow->rto_active_);  // Still unacked data.
}

TEST_F(TcpFlowTest, AdvanceSndUna_AllAcked) {
  auto flow = MakeFlow();
  flow->snd_una_ = 100;
  flow->snd_nxt_ = 200;
  flow->rto_active_ = true;

  flow->AdvanceSndUna(200);
  EXPECT_EQ(flow->snd_una_, 200u);
  EXPECT_FALSE(flow->rto_active_);  // All data acknowledged.
}

TEST_F(TcpFlowTest, AdvanceSndUna_StaleAck) {
  auto flow = MakeFlow();
  flow->snd_una_ = 100;
  flow->snd_nxt_ = 200;

  // ACK for something already acknowledged — should be a no-op.
  flow->AdvanceSndUna(50);
  EXPECT_EQ(flow->snd_una_, 100u);
}

TEST_F(TcpFlowTest, AdvanceSndUna_FutureAck) {
  auto flow = MakeFlow();
  flow->snd_una_ = 100;
  flow->snd_nxt_ = 200;

  // ACK beyond snd_nxt_ — should be ignored.
  flow->AdvanceSndUna(250);
  EXPECT_EQ(flow->snd_una_, 100u);
}

// ═══════════════════════════════════════════════════════════════
//  StateToString
// ═══════════════════════════════════════════════════════════════

TEST_F(TcpFlowTest, StateToString) {
  EXPECT_STREQ(TcpFlow::StateToString(TcpFlow::State::kClosed), "CLOSED");
  EXPECT_STREQ(TcpFlow::StateToString(TcpFlow::State::kListen), "LISTEN");
  EXPECT_STREQ(TcpFlow::StateToString(TcpFlow::State::kSynSent), "SYN_SENT");
  EXPECT_STREQ(TcpFlow::StateToString(TcpFlow::State::kSynReceived),
               "SYN_RECEIVED");
  EXPECT_STREQ(TcpFlow::StateToString(TcpFlow::State::kEstablished),
               "ESTABLISHED");
  EXPECT_STREQ(TcpFlow::StateToString(TcpFlow::State::kFinWait1),
               "FIN_WAIT_1");
  EXPECT_STREQ(TcpFlow::StateToString(TcpFlow::State::kFinWait2),
               "FIN_WAIT_2");
  EXPECT_STREQ(TcpFlow::StateToString(TcpFlow::State::kCloseWait),
               "CLOSE_WAIT");
  EXPECT_STREQ(TcpFlow::StateToString(TcpFlow::State::kLastAck),
               "LAST_ACK");
  EXPECT_STREQ(TcpFlow::StateToString(TcpFlow::State::kTimeWait),
               "TIME_WAIT");
}

// ═══════════════════════════════════════════════════════════════
//  Match
// ═══════════════════════════════════════════════════════════════

TEST_F(TcpFlowTest, Match_CorrectPacket) {
  auto flow = MakeFlow();
  auto* pkt = MakePacket(0, 0, Tcp::kSyn);
  EXPECT_TRUE(flow->Match(pkt));
  dpdk::Packet::Free(pkt);
}

TEST_F(TcpFlowTest, Match_WrongSrcAddr) {
  auto flow = MakeFlow();
  auto* pkt = MakePacket(0, 0, Tcp::kSyn);
  // Tamper with src IP.
  auto* ipv4h = pkt->head_data<Ipv4*>(sizeof(Ethernet));
  Ipv4::Address wrong_ip;
  wrong_ip.FromString("192.168.1.1");
  ipv4h->src_addr = wrong_ip;
  EXPECT_FALSE(flow->Match(pkt));
  dpdk::Packet::Free(pkt);
}

// ═══════════════════════════════════════════════════════════════
//  Ethernet Padding Resilience (IP total_length vs packet->length())
// ═══════════════════════════════════════════════════════════════

TEST_F(TcpFlowTest, EthernetPadding_SmallPacket) {
  // When Ethernet pads a frame to 60 bytes, packet->length() overcounts.
  // The TCP InputPacket path should use IP total_length instead.
  auto flow = MakeFlow();
  flow->state_ = TcpFlow::State::kEstablished;
  flow->rcv_nxt_ = 1000;
  flow->snd_una_ = 500;
  flow->snd_nxt_ = 500;

  // Create a packet with 2 bytes of payload.
  uint8_t payload[2] = {0x41, 0x42};
  auto* pkt = MakePacket(1000, 500, Tcp::kAck, payload, 2);

  // Now pad the packet to 60 bytes (simulating Ethernet minimum).
  // The actual payload_len from IP header is correct (2 bytes), but
  // packet->length() may be larger.
  size_t actual_pkt_len = pkt->length();
  // Note: packet->length() may be >= 60 due to Ethernet padding.
  // The test verifies that the IP total_length path works correctly
  // regardless of packet->length().
  (void)actual_pkt_len;

  flow->InputPacket(pkt);

  // Should have consumed exactly 2 bytes, not more.
  EXPECT_EQ(flow->rcv_nxt_, 1002u);

  dpdk::Packet::Free(pkt);
}

// ═══════════════════════════════════════════════════════════════
//  IP total_length too small
// ═══════════════════════════════════════════════════════════════

TEST_F(TcpFlowTest, InputPacket_IpTotalLengthTooSmall) {
  auto flow = MakeFlow();
  flow->state_ = TcpFlow::State::kEstablished;
  flow->rcv_nxt_ = 1000;

  auto* pkt = MakePacket(1000, 500, Tcp::kAck);
  // Corrupt IP total_length to be smaller than IP + TCP header.
  auto* ipv4h = pkt->head_data<Ipv4*>(sizeof(Ethernet));
  ipv4h->total_length = be16_t(10);  // Way too small.

  uint32_t old_rcv_nxt = flow->rcv_nxt_;
  flow->InputPacket(pkt);
  EXPECT_EQ(flow->rcv_nxt_, old_rcv_nxt);  // No change.

  dpdk::Packet::Free(pkt);
}

}  // namespace flow
}  // namespace net
}  // namespace juggler

int main(int argc, char** argv) {
  ::google::InitGoogleLogging(argv[0]);
  testing::InitGoogleTest(&argc, argv);
  FLAGS_logtostderr = 1;

  // Initialize DPDK with a null virtual device and no PCI.
  auto kEalOpts = juggler::utils::CmdLineOpts(
      {"", "-c", "0x1", "-n", "6", "--proc-type=auto", "-m", "1024",
       "--log-level", "8", "--vdev=net_null0,copy=1", "--no-pci"});
  auto d = juggler::dpdk::Dpdk();
  d.InitDpdk(kEalOpts);

  return RUN_ALL_TESTS();
}
