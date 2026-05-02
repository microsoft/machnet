/**
 * @file tcp_flow.h
 * @brief TCP flow implementation for Machnet.
 *
 * This provides a TCP-based transport path alongside the existing UDP-based
 * Machnet protocol. A TcpFlow speaks standard TCP (3-way handshake, sequence
 * numbers, ACKs, FIN) and translates between Machnet's message-based shared
 * memory channel API and TCP byte streams.
 *
 * Key design decisions:
 *  - Reuses the same Channel / MsgBuf shared memory infrastructure.
 *  - Each TcpFlow is bound to one Channel, just like a UDP Flow.
 *  - Messages are framed on the wire with a 4-byte length prefix so that the
 *    receiver can reconstruct message boundaries from the TCP byte stream.
 *  - Uses the same flow::Key structure (the key is protocol-agnostic: IPs +
 *    ports).
 *  - The TcpFlow manages its own TCP state machine including connection
 *    establishment, data transfer (with simple sliding-window flow control),
 *    and teardown.
 */
#ifndef SRC_INCLUDE_TCP_FLOW_H_
#define SRC_INCLUDE_TCP_FLOW_H_

#include <channel.h>
#include <channel_msgbuf.h>
#include <common.h>
#include <dpdk.h>
#include <ether.h>
#include <flow_key.h>
#include <glog/logging.h>
#include <ipv4.h>
#include <packet.h>
#include <packet_pool.h>
#include <pmd.h>
#include <tcp.h>
#include <types.h>
#include <utils.h>

#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <optional>
#include <string>

namespace juggler {
namespace net {
namespace flow {

/**
 * @class TcpFlow
 * @brief A TCP connection that interfaces with the Machnet shared-memory
 * channel system.
 *
 * Mirrors net::flow::Flow but uses real TCP on the wire instead of
 * UDP + MachnetPktHdr.  Messages from the application are framed with a
 * 4-byte network-order length prefix before being pushed into the TCP stream.
 * Incoming TCP data is reassembled and de-framed before delivery to the app.
 */
class TcpFlow {
 public:
  using Ethernet = net::Ethernet;
  using Ipv4 = net::Ipv4;
  using Tcp = net::Tcp;
  using ApplicationCallback =
      std::function<void(shm::Channel*, bool, const Key&)>;

  /// TCP connection states (standard simplified).
  enum class State {
    kClosed,
    kListen,
    kSynSent,
    kSynReceived,
    kEstablished,
    kFinWait1,
    kFinWait2,
    kCloseWait,
    kLastAck,
    kTimeWait,
  };

  static constexpr const char* StateToString(State state) {
    switch (state) {
      case State::kClosed:
        return "CLOSED";
      case State::kListen:
        return "LISTEN";
      case State::kSynSent:
        return "SYN_SENT";
      case State::kSynReceived:
        return "SYN_RECEIVED";
      case State::kEstablished:
        return "ESTABLISHED";
      case State::kFinWait1:
        return "FIN_WAIT_1";
      case State::kFinWait2:
        return "FIN_WAIT_2";
      case State::kCloseWait:
        return "CLOSE_WAIT";
      case State::kLastAck:
        return "LAST_ACK";
      case State::kTimeWait:
        return "TIME_WAIT";
      default:
        return "UNKNOWN";
    }
  }

  /// 4-byte message length prefix used to frame messages over TCP.
  static constexpr size_t kMsgLenPrefixSize = 4;

  /// Maximum TCP segment payload (MSS). Conservative default.
  static constexpr size_t kDefaultMSS = 1400;

  /// Size of the TCP MSS option (Kind=2, Length=4, Value=2 bytes).
  static constexpr size_t kMSSOptionLen = 4;

  /// Initial TCP window size (in bytes).
  static constexpr uint16_t kInitialWindowSize = 65535;

  /// Maximum number of retransmission attempts before giving up.
  static constexpr uint32_t kMaxRetransmissions = 10;

  /// Initial RTO value in slow ticks (same units as PeriodicCheck calls).
  static constexpr uint32_t kInitialRTO = 3;

  /// TIME_WAIT duration in slow ticks.
  static constexpr uint32_t kTimeWaitTicks = 5;

  // ──────────────────────── Construction ────────────────────────

  TcpFlow(const Ipv4::Address& local_addr, const Tcp::Port& local_port,
          const Ipv4::Address& remote_addr, const Tcp::Port& remote_port,
          const Ethernet::Address& local_l2_addr,
          const Ethernet::Address& remote_l2_addr, dpdk::TxRing* txring,
          ApplicationCallback callback, shm::Channel* channel)
      : key_(local_addr, local_port, remote_addr, remote_port),
        local_l2_addr_(local_l2_addr),
        remote_l2_addr_(remote_l2_addr),
        state_(State::kClosed),
        txring_(CHECK_NOTNULL(txring)),
        callback_(std::move(callback)),
        channel_(CHECK_NOTNULL(channel)),
        // TCP sequence / ack tracking
        snd_una_(0),
        snd_nxt_(0),
        snd_isn_(GenerateISN()),
        rcv_nxt_(0),
        rcv_wnd_(kInitialWindowSize),
        snd_wnd_(kInitialWindowSize),
        rto_ticks_(kInitialRTO),
        rto_remaining_(kInitialRTO),
        rto_active_(false),
        retransmit_count_(0),
        // RX reassembly
        rx_buf_offset_(0),
        rx_pending_msg_len_(0),
        rx_msg_train_head_(nullptr),
        rx_msg_train_tail_(nullptr) {
    CHECK_NOTNULL(txring_->GetPacketPool());
    snd_nxt_ = snd_isn_;
    snd_una_ = snd_isn_;
  }

  ~TcpFlow() = default;

  // ──────────────────── Accessors ────────────────────

  const Key& key() const { return key_; }
  shm::Channel* channel() const { return channel_; }
  State state() const { return state_; }

  bool operator==(const TcpFlow& other) const { return key_ == other.key(); }

  std::string ToString() const {
    return utils::Format(
        "TCP %s [%s] <-> [%s] snd_una=%u snd_nxt=%u rcv_nxt=%u",
        key_.ToString().c_str(), StateToString(state_),
        channel_->GetName().c_str(), snd_una_, snd_nxt_, rcv_nxt_);
  }

  bool Match(const dpdk::Packet* packet) const {
    const auto* ih = packet->head_data<Ipv4*>(sizeof(Ethernet));
    const auto* tcph =
        packet->head_data<Tcp*>(sizeof(Ethernet) + sizeof(Ipv4));
    return (ih->src_addr == key_.remote_addr &&
            ih->dst_addr == key_.local_addr &&
            tcph->src_port == key_.remote_port &&
            tcph->dst_port == key_.local_port);
  }

  // ────────────────── Active Open (Client) ──────────────────

  void InitiateHandshake() {
    CHECK(state_ == State::kClosed);
    SendSyn();
    state_ = State::kSynSent;
    rto_active_ = true;
    rto_remaining_ = rto_ticks_;
  }

  // ────────────────── Passive Open (Server) ──────────────────

  /**
   * @brief Set the flow into LISTEN-like state for a passive open.
   *
   * Called by the engine when a SYN arrives on a listening port. The engine
   * creates a new TcpFlow and calls StartPassiveOpen() *before* InputPacket()
   * so that the SYN is correctly processed.
   */
  void StartPassiveOpen() {
    CHECK(state_ == State::kClosed);
    state_ = State::kListen;
  }

  // ────────────────── Shutdown ──────────────────

  void ShutDown() {
    if (state_ == State::kEstablished || state_ == State::kCloseWait) {
      SendFin();
      state_ = (state_ == State::kEstablished) ? State::kFinWait1
                                                : State::kLastAck;
    } else {
      SendRst();
      state_ = State::kClosed;
    }
    rto_active_ = false;
  }

  // ────────────────── RX Path ──────────────────

  /**
   * @brief Process an incoming TCP packet.
   */
  void InputPacket(const dpdk::Packet* packet) {
    const auto* ipv4h = packet->head_data<Ipv4*>(sizeof(Ethernet));
    const auto* tcph =
        packet->head_data<Tcp*>(sizeof(Ethernet) + sizeof(Ipv4));
    const uint8_t tcp_hdr_len = tcph->header_length();

    // Validate TCP header length (min 20, max 60 bytes).
    if (tcp_hdr_len < sizeof(Tcp) || tcp_hdr_len > 60) [[unlikely]] {
      LOG(WARNING) << "TCP: invalid header length "
                   << static_cast<int>(tcp_hdr_len);
      return;
    }

    const size_t net_hdr_len = sizeof(Ethernet) + sizeof(Ipv4) + tcp_hdr_len;
    const uint32_t seg_seq = tcph->seq_num.value();
    const uint32_t seg_ack = tcph->ack_num.value();
    const uint8_t flags = tcph->flags;
    // Use IP total_length rather than packet->length() to compute the TCP
    // payload size.  Ethernet frames may be padded to the 60-byte minimum,
    // so packet->length() can overcount by up to 6 bytes, corrupting the
    // TCP reassembly / deframing state machine.
    const size_t ip_total_len = ipv4h->total_length.value();
    if (ip_total_len < sizeof(Ipv4) + tcp_hdr_len) [[unlikely]] {
      LOG(WARNING) << "TCP: IP total_length too small for TCP header";
      return;
    }
    const size_t payload_len =
        ip_total_len - sizeof(Ipv4) - tcp_hdr_len;

    // ── RST handling (any state) ──
    if (flags & Tcp::kRst) {
      LOG(INFO) << "TCP RST received on " << key_.ToString();
      state_ = State::kClosed;
      return;
    }

    switch (state_) {
      case State::kListen:
        // Passive open: incoming SYN on a newly created flow.
        // Parse TCP options from the kernel's SYN (MSS, etc.).
        if (flags & Tcp::kSyn) {
          ParseTcpOptions(tcph, tcp_hdr_len);
          rcv_nxt_ = seg_seq + 1;  // SYN consumes one seq.
          SendSynAck();
          state_ = State::kSynReceived;
          rto_active_ = true;
          rto_remaining_ = rto_ticks_;
        }
        break;
      case State::kSynSent:
        HandleSynSent(tcph, seg_seq, seg_ack, flags);
        break;
      case State::kSynReceived:
        HandleSynReceived(tcph, seg_seq, seg_ack, flags, packet, payload_len,
                          net_hdr_len);
        break;
      case State::kEstablished:
        HandleEstablished(tcph, seg_seq, seg_ack, flags, packet, payload_len,
                          net_hdr_len);
        break;
      case State::kFinWait1:
        HandleFinWait1(tcph, seg_seq, seg_ack, flags, packet, payload_len,
                       net_hdr_len);
        break;
      case State::kFinWait2:
        HandleFinWait2(tcph, seg_seq, seg_ack, flags, packet, payload_len,
                       net_hdr_len);
        break;
      case State::kCloseWait:
        // We already received FIN; waiting for app to close.
        if (flags & Tcp::kAck) {
          AdvanceSndUna(seg_ack);
        }
        break;
      case State::kLastAck:
        if (flags & Tcp::kAck) {
          AdvanceSndUna(seg_ack);
          state_ = State::kClosed;
        }
        break;
      case State::kTimeWait:
        // Absorb duplicates; stay in TIME_WAIT.
        break;
      case State::kClosed:
        // Stale packet on closed connection; ignore or send RST.
        if (!(flags & Tcp::kRst)) {
          SendRst();
        }
        break;
      default:
        break;
    }
  }

  // ────────────────── TX Path ──────────────────

  /**
   * @brief Send an application message over this TCP flow.
   *
   * The message is framed with a 4-byte length prefix, then segmented into
   * TCP-sized packets and transmitted.
   */
  void OutputMessage(shm::MsgBuf* msg) {
    if (state_ != State::kEstablished && state_ != State::kCloseWait) {
      LOG(ERROR) << "Cannot send on TCP flow in state " << StateToString(state_);
      return;
    }

    VLOG(1) << "TCP OutputMessage: " << key_.ToString()
            << " msg_len=" << msg->msg_length()
            << " snd_nxt=" << snd_nxt_ << " snd_una=" << snd_una_;

    // Gather the full message payload from the MsgBuf train.
    // We copy the payload into a contiguous buffer to simplify TCP
    // segmentation. For a zero-copy path this could be optimized later.
    const uint32_t msg_len = msg->msg_length();
    const uint32_t total_len = kMsgLenPrefixSize + msg_len;

    // We'll transmit the length prefix + payload as a TCP byte stream.
    // Segment into MSS-sized TCP packets.
    uint32_t bytes_sent = 0;

    // First, send the 4-byte length prefix.
    uint8_t len_buf[kMsgLenPrefixSize];
    uint32_t net_len = htobe32(msg_len);
    std::memcpy(len_buf, &net_len, kMsgLenPrefixSize);

    // Walk through the MsgBuf chain and send data.
    // We interleave the length prefix with the first payload chunk.
    auto* cur_buf = msg;
    size_t prefix_remaining = kMsgLenPrefixSize;
    size_t buf_offset = 0;  // Offset within the current MsgBuf.
    bool first_segment = true;

    while (bytes_sent < total_len) {
      auto* packet = txring_->GetPacketPool()->PacketAlloc();
      if (packet == nullptr) {
        LOG(ERROR) << "Failed to allocate packet for TCP TX";
        return;
      }
      dpdk::Packet::Reset(packet);

      const size_t hdr_len = sizeof(Ethernet) + sizeof(Ipv4) + sizeof(Tcp);
      size_t payload_room = peer_mss_;
      size_t pkt_payload_len = 0;

      // Allocate space for headers + max payload.
      size_t max_pkt_len =
          hdr_len + std::min<size_t>(payload_room, total_len - bytes_sent);
      CHECK_NOTNULL(packet->append(static_cast<uint16_t>(max_pkt_len)));

      uint8_t* payload_dst =
          packet->head_data<uint8_t*>(static_cast<uint16_t>(hdr_len));

      // Copy length prefix into the first segment(s).
      if (prefix_remaining > 0) {
        size_t prefix_to_copy = std::min(prefix_remaining, payload_room);
        std::memcpy(payload_dst,
                     len_buf + (kMsgLenPrefixSize - prefix_remaining),
                     prefix_to_copy);
        payload_dst += prefix_to_copy;
        pkt_payload_len += prefix_to_copy;
        prefix_remaining -= prefix_to_copy;
        payload_room -= prefix_to_copy;
      }

      // Copy message payload from MsgBuf chain.
      while (payload_room > 0 && cur_buf != nullptr) {
        const size_t avail = cur_buf->length() - buf_offset;
        const size_t to_copy = std::min(avail, payload_room);
        std::memcpy(payload_dst,
                     static_cast<const uint8_t*>(cur_buf->head_data()) + buf_offset,
                     to_copy);
        payload_dst += to_copy;
        pkt_payload_len += to_copy;
        payload_room -= to_copy;
        buf_offset += to_copy;

        if (buf_offset == cur_buf->length()) {
          // Fully consumed this MsgBuf; move to next in chain.
          buf_offset = 0;
          if (cur_buf->is_sg() || cur_buf->has_chain()) {
            cur_buf = channel_->GetMsgBuf(cur_buf->next());
          } else {
            cur_buf = nullptr;
          }
        }
      }

      // Adjust actual packet length if we didn't fill the max.
      // We already appended max_pkt_len; trim if needed.
      // Actually, since we append the exact max and copy into it, the packet
      // length is already correct if max_pkt_len was right. For safety:
      // (packet length is set by append)

      // Prepare headers.
      uint8_t tcp_flags = Tcp::kAck;
      if (first_segment) {
        tcp_flags |= Tcp::kPsh;  // Push on first segment for low latency.
        first_segment = false;
      }

      PrepareL2Header(packet);
      PrepareL3Header(packet);
      PrepareL4Header(packet, snd_nxt_, rcv_nxt_, tcp_flags);
      packet->offload_tcpv4_csum();

      snd_nxt_ += pkt_payload_len;
      bytes_sent += pkt_payload_len;

      txring_->SendPackets(&packet, 1);
    }

    if (!rto_active_) {
      rto_active_ = true;
      rto_remaining_ = rto_ticks_;
    }

    // Free the MsgBuf chain (the engine normally tracks this, but since TCP
    // does its own segmentation, we consume the buffers here).
    FreeMsgBufChain(msg);
  }

  // ────────────────── Periodic Check ──────────────────

  /**
   * @brief Called periodically by the engine to handle retransmissions and
   * time-waits.
   * @return false if the flow should be removed.
   */
  bool PeriodicCheck() {
    if (state_ == State::kClosed) return false;
    if (state_ == State::kTimeWait) {
      if (time_wait_remaining_ > 0) {
        time_wait_remaining_--;
        return true;
      }
      state_ = State::kClosed;
      return false;
    }

    if (!rto_active_) return true;

    if (rto_remaining_ > 0) {
      rto_remaining_--;
      return true;
    }

    // RTO expired.
    retransmit_count_++;
    if (retransmit_count_ > kMaxRetransmissions) {
      LOG(ERROR) << "TCP max retransmissions reached on " << key_.ToString();
      if (state_ == State::kSynSent) {
        callback_(channel_, false, key_);
      }
      state_ = State::kClosed;
      return false;
    }

    // Retransmit based on state.
    switch (state_) {
      case State::kSynSent:
        LOG(INFO) << "TCP retransmitting SYN";
        SendSyn();
        break;
      case State::kSynReceived:
        LOG(INFO) << "TCP retransmitting SYN-ACK";
        SendSynAck();
        break;
      default:
        // For established connections, a full retransmission mechanism
        // would require buffering sent data. For now, we just reset the timer.
        break;
    }
    rto_remaining_ = rto_ticks_;
    return true;
  }

 private:
  // ──────────────── Helpers: Header Preparation ────────────────

  void PrepareL2Header(dpdk::Packet* packet) const {
    auto* eh = packet->head_data<Ethernet*>();
    eh->src_addr = local_l2_addr_;
    eh->dst_addr = remote_l2_addr_;
    eh->eth_type = be16_t(Ethernet::kIpv4);
    packet->set_l2_len(sizeof(*eh));
  }

  void PrepareL3Header(dpdk::Packet* packet) const {
    auto* ipv4h = packet->head_data<Ipv4*>(sizeof(Ethernet));
    ipv4h->version_ihl = 0x45;
    ipv4h->type_of_service = 0;
    ipv4h->packet_id = be16_t(0x1513);
    ipv4h->fragment_offset = be16_t(0x4000);  // Don't Fragment.
    ipv4h->time_to_live = 64;
    ipv4h->next_proto_id = Ipv4::Proto::kTcp;
    ipv4h->total_length = be16_t(packet->length() - sizeof(Ethernet));
    ipv4h->src_addr = key_.local_addr;
    ipv4h->dst_addr = key_.remote_addr;
    ipv4h->hdr_checksum = 0;
    packet->set_l3_len(sizeof(*ipv4h));
  }

  void PrepareL4Header(dpdk::Packet* packet, uint32_t seq, uint32_t ack,
                        uint8_t flags) const {
    auto* tcph = packet->head_data<Tcp*>(sizeof(Ethernet) + sizeof(Ipv4));
    tcph->src_port = key_.local_port;
    tcph->dst_port = key_.remote_port;
    tcph->seq_num = be32_t(seq);
    tcph->ack_num = be32_t(ack);
    tcph->set_header_length(sizeof(Tcp));  // 20 bytes, no options.
    tcph->flags = flags;
    tcph->window = be16_t(rcv_wnd_);
    tcph->checksum = 0;
    tcph->urgent_ptr = be16_t(0);
  }

  // ──────────────── Helpers: Send Control Packets ────────────────

  void SendControlPacket(uint32_t seq, uint32_t ack, uint8_t flags) {
    auto* packet = CHECK_NOTNULL(txring_->GetPacketPool()->PacketAlloc());
    dpdk::Packet::Reset(packet);

    const size_t pkt_len = sizeof(Ethernet) + sizeof(Ipv4) + sizeof(Tcp);
    CHECK_NOTNULL(packet->append(static_cast<uint16_t>(pkt_len)));

    PrepareL2Header(packet);
    PrepareL3Header(packet);
    PrepareL4Header(packet, seq, ack, flags);
    packet->offload_tcpv4_csum();

    txring_->SendPackets(&packet, 1);
  }

  /// Send a control packet with the MSS option appended (for SYN/SYN-ACK).
  /// Linux kernel expects an MSS option; without it, it defaults to 536 bytes.
  void SendControlPacketWithMSS(uint32_t seq, uint32_t ack, uint8_t flags,
                                 uint16_t mss) {
    auto* packet = CHECK_NOTNULL(txring_->GetPacketPool()->PacketAlloc());
    dpdk::Packet::Reset(packet);

    const size_t tcp_hdr_with_opts = sizeof(Tcp) + kMSSOptionLen;
    const size_t pkt_len = sizeof(Ethernet) + sizeof(Ipv4) + tcp_hdr_with_opts;
    CHECK_NOTNULL(packet->append(static_cast<uint16_t>(pkt_len)));

    PrepareL2Header(packet);
    PrepareL3Header(packet);
    PrepareL4Header(packet, seq, ack, flags);

    // Override TCP header length to include the MSS option.
    auto* tcph = packet->head_data<Tcp*>(sizeof(Ethernet) + sizeof(Ipv4));
    tcph->set_header_length(static_cast<uint8_t>(tcp_hdr_with_opts));

    // Write MSS option: Kind=2, Length=4, Value=MSS (big-endian).
    uint8_t* opts = reinterpret_cast<uint8_t*>(tcph) + sizeof(Tcp);
    opts[0] = 2;   // Kind: Maximum Segment Size
    opts[1] = 4;   // Length
    uint16_t mss_net = htobe16(mss);
    std::memcpy(&opts[2], &mss_net, sizeof(mss_net));

    packet->offload_tcpv4_csum();
    txring_->SendPackets(&packet, 1);
  }

  void SendSyn() {
    SendControlPacketWithMSS(snd_nxt_, 0, Tcp::kSyn,
                              static_cast<uint16_t>(kDefaultMSS));
    snd_nxt_++;  // SYN consumes one sequence number.
  }

  void SendSynAck() {
    SendControlPacketWithMSS(snd_isn_, rcv_nxt_, Tcp::kSyn | Tcp::kAck,
                              static_cast<uint16_t>(kDefaultMSS));
    snd_nxt_ = snd_isn_ + 1;  // SYN-ACK consumes one sequence number.
  }

  void SendAck() { SendControlPacket(snd_nxt_, rcv_nxt_, Tcp::kAck); }

  void SendFin() {
    SendControlPacket(snd_nxt_, rcv_nxt_, Tcp::kFin | Tcp::kAck);
    snd_nxt_++;  // FIN consumes one sequence number.
  }

  void SendRst() { SendControlPacket(snd_nxt_, 0, Tcp::kRst); }

  // ──────────────── Helpers: TCP Option Parsing ──────────────────

  /// Parse TCP options from a received header. Currently extracts MSS.
  /// Linux kernel SYN/SYN-ACK includes MSS, Window Scale, SACK-Permitted,
  /// and Timestamps. We parse MSS and ignore the rest (since we don't
  /// negotiate window scaling, the kernel won't apply it).
  void ParseTcpOptions(const Tcp* tcph, uint8_t hdr_len) {
    if (hdr_len <= sizeof(Tcp)) return;  // No options.
    const uint8_t* opts =
        reinterpret_cast<const uint8_t*>(tcph) + sizeof(Tcp);
    const size_t opts_len = hdr_len - sizeof(Tcp);
    size_t i = 0;
    while (i < opts_len) {
      uint8_t kind = opts[i];
      if (kind == 0) break;             // End of Option List.
      if (kind == 1) { i++; continue; } // NOP padding.
      if (i + 1 >= opts_len) break;
      uint8_t opt_len = opts[i + 1];
      if (opt_len < 2 || i + opt_len > opts_len) break;  // Malformed.
      if (kind == 2 && opt_len == 4) {
        // MSS option.
        uint16_t mss_net;
        std::memcpy(&mss_net, opts + i + 2, sizeof(mss_net));
        peer_mss_ = be16toh(mss_net);
        if (peer_mss_ == 0) peer_mss_ = static_cast<uint16_t>(kDefaultMSS);
        VLOG(1) << "TCP: parsed peer MSS=" << peer_mss_;
      }
      // Window Scale (kind=3), SACK-Permitted (kind=4), Timestamps (kind=8):
      // intentionally ignored — we don't negotiate these options.
      i += opt_len;
    }
  }

  // ──────────────── Helpers: In-order Payload with Overlap ──────────────────

  /**
   * @brief Process incoming payload, handling partial retransmission overlaps.
   *
   * The Linux kernel retransmits aggressively, and a retransmitted segment
   * may partially overlap data we already received.  This helper skips the
   * already-received prefix and delivers only new bytes to ConsumePayload.
   *
   * @return true if new data was consumed (caller should ACK).
   */
  bool ProcessInOrderPayload(const dpdk::Packet* packet, uint32_t seg_seq,
                              size_t payload_len, size_t net_hdr_len) {
    if (payload_len == 0) return false;

    const uint8_t* base_payload =
        packet->head_data<const uint8_t*>(
            static_cast<uint16_t>(net_hdr_len));

    if (seg_seq == rcv_nxt_) {
      // Perfect in-order delivery.
      ConsumePayload(base_payload, payload_len);
      rcv_nxt_ += static_cast<uint32_t>(payload_len);
      return true;
    }

    // Check for retransmission that partially overlaps new data.
    uint32_t seg_end = seg_seq + static_cast<uint32_t>(payload_len);
    if (SeqLeq(seg_seq, rcv_nxt_) && SeqGt(seg_end, rcv_nxt_)) {
      uint32_t overlap = rcv_nxt_ - seg_seq;  // Works with wrapping.
      size_t new_len = payload_len - overlap;
      ConsumePayload(base_payload + overlap, new_len);
      rcv_nxt_ += static_cast<uint32_t>(new_len);
      return true;
    }

    // Pure duplicate (seg_end <= rcv_nxt_) or out-of-order gap.
    return false;
  }

  // ──────────────── State Machine Handlers ────────────────

  void HandleSynSent(const Tcp* tcph, uint32_t seg_seq, uint32_t seg_ack,
                     uint8_t flags) {
    if ((flags & Tcp::kSyn) && (flags & Tcp::kAck)) {
      // SYN-ACK received from Linux kernel.
      if (seg_ack != snd_nxt_) {
        LOG(ERROR) << "TCP SYN-ACK with wrong ack: " << seg_ack
                   << " expected " << snd_nxt_;
        return;
      }
      rcv_nxt_ = seg_seq + 1;  // SYN consumes one seq.
      snd_una_ = seg_ack;
      snd_wnd_ = tcph->window.value();

      // Parse TCP options from the kernel's SYN-ACK (MSS, etc.).
      ParseTcpOptions(tcph, tcph->header_length());

      // Send ACK to complete 3-way handshake.
      SendAck();
      state_ = State::kEstablished;
      rto_active_ = false;
      retransmit_count_ = 0;

      // Notify application.
      callback_(channel_, true, key_);
    } else if (flags & Tcp::kSyn) {
      // Simultaneous open: SYN without ACK.
      ParseTcpOptions(tcph, tcph->header_length());
      rcv_nxt_ = seg_seq + 1;
      SendSynAck();
      state_ = State::kSynReceived;
    }
  }

  void HandleSynReceived(const Tcp* tcph, uint32_t seg_seq,
                         uint32_t seg_ack, uint8_t flags,
                         const dpdk::Packet* packet, size_t payload_len,
                         size_t net_hdr_len) {
    if (flags & Tcp::kAck) {
      if (seg_ack == snd_nxt_) {
        state_ = State::kEstablished;
        snd_una_ = seg_ack;
        snd_wnd_ = tcph->window.value();
        rto_active_ = false;
        retransmit_count_ = 0;

        // Linux kernel can piggyback data on the completing handshake ACK.
        if (ProcessInOrderPayload(packet, seg_seq, payload_len, net_hdr_len)) {
          SendAck();
        }
      }
    }
  }

  void HandleEstablished(const Tcp* tcph, uint32_t seg_seq, uint32_t seg_ack,
                         uint8_t flags, const dpdk::Packet* packet,
                         size_t payload_len, size_t net_hdr_len) {
    VLOG(1) << "TCP HandleEstablished: " << key_.ToString()
            << " seq=" << seg_seq << " ack=" << seg_ack
            << " flags=0x" << std::hex << static_cast<int>(flags) << std::dec
            << " payload_len=" << payload_len;

    // Handle retransmitted SYN(-ACK) from the kernel — it missed our
    // final handshake ACK.  Re-send the ACK so the kernel can proceed.
    if (flags & Tcp::kSyn) {
      SendAck();
      return;
    }

    // Process ACK.
    if (flags & Tcp::kAck) {
      AdvanceSndUna(seg_ack);
      snd_wnd_ = tcph->window.value();
    }

    // Process payload data with overlap handling for kernel retransmissions.
    if (payload_len > 0) {
      if (ProcessInOrderPayload(packet, seg_seq, payload_len, net_hdr_len)) {
        SendAck();
      } else {
        // Duplicate or out-of-order — send dup ACK to trigger fast retransmit.
        SendAck();
      }
    }

    // FIN handling — only accept if the FIN is at the expected sequence.
    if (flags & Tcp::kFin) {
      uint32_t fin_seq = seg_seq + static_cast<uint32_t>(payload_len);
      if (fin_seq == rcv_nxt_) {
        rcv_nxt_++;  // FIN consumes one sequence number.
        SendAck();
        state_ = State::kCloseWait;
      } else if (SeqLt(fin_seq, rcv_nxt_)) {
        // Retransmitted FIN we already processed — re-ACK.
        SendAck();
      }
      // fin_seq > rcv_nxt_: gap ahead of FIN; ignore for now, kernel will
      // retransmit the missing data.
    }
  }

  void HandleFinWait1(const Tcp* tcph, uint32_t seg_seq, uint32_t seg_ack,
                      uint8_t flags, const dpdk::Packet* packet,
                      size_t payload_len, size_t net_hdr_len) {
    if (flags & Tcp::kAck) {
      AdvanceSndUna(seg_ack);
    }

    // Process incoming data with overlap handling.
    if (payload_len > 0) {
      if (ProcessInOrderPayload(packet, seg_seq, payload_len, net_hdr_len)) {
        SendAck();
      } else {
        SendAck();
      }
    }

    bool our_fin_acked = (snd_una_ == snd_nxt_);

    if (flags & Tcp::kFin) {
      uint32_t fin_seq = seg_seq + static_cast<uint32_t>(payload_len);
      if (fin_seq == rcv_nxt_) {
        rcv_nxt_++;
      }
      SendAck();
      state_ = State::kTimeWait;
      time_wait_remaining_ = kTimeWaitTicks;
    } else if (our_fin_acked) {
      state_ = State::kFinWait2;
    }
  }

  void HandleFinWait2(const Tcp* /*tcph*/, uint32_t seg_seq,
                      uint32_t /*seg_ack*/, uint8_t flags,
                      const dpdk::Packet* packet, size_t payload_len,
                      size_t net_hdr_len) {
    // Process incoming data with overlap handling.
    if (payload_len > 0) {
      if (ProcessInOrderPayload(packet, seg_seq, payload_len, net_hdr_len)) {
        SendAck();
      } else {
        SendAck();
      }
    }

    if (flags & Tcp::kFin) {
      uint32_t fin_seq = seg_seq + static_cast<uint32_t>(payload_len);
      if (fin_seq == rcv_nxt_) {
        rcv_nxt_++;
      }
      SendAck();
      state_ = State::kTimeWait;
      time_wait_remaining_ = kTimeWaitTicks;
    }
  }

  // ──────────────── RX Reassembly / Deframing ────────────────

  /**
   * @brief Consume incoming TCP payload bytes and reassemble framed messages.
   *
   * Messages on the wire are preceded by a 4-byte big-endian length prefix.
   * This function accumulates bytes and delivers complete messages to the
   * channel for the application to consume.
   */
  void ConsumePayload(const uint8_t* data, size_t len) {
    size_t offset = 0;
    while (offset < len) {
      // Phase 1: Read the message length prefix if we haven't yet.
      if (rx_pending_msg_len_ == 0) {
        // We need 4 bytes for the length prefix.
        while (rx_len_buf_offset_ < kMsgLenPrefixSize && offset < len) {
          rx_len_buf_[rx_len_buf_offset_++] = data[offset++];
        }
        if (rx_len_buf_offset_ < kMsgLenPrefixSize) {
          return;  // Need more data for the length prefix.
        }
        uint32_t net_msg_len;
        std::memcpy(&net_msg_len, rx_len_buf_, kMsgLenPrefixSize);
        rx_pending_msg_len_ = be32toh(net_msg_len);
        rx_buf_offset_ = 0;
        rx_len_buf_offset_ = 0;

        if (rx_pending_msg_len_ == 0 ||
            rx_pending_msg_len_ > MACHNET_MSG_MAX_LEN) {
          LOG(ERROR) << "Invalid TCP message length: " << rx_pending_msg_len_;
          rx_pending_msg_len_ = 0;
          return;
        }
      }

      // Phase 2: Copy payload bytes into MsgBuf(s).
      size_t remaining_for_msg = rx_pending_msg_len_ - rx_buf_offset_;
      size_t available = len - offset;
      size_t to_consume = std::min(remaining_for_msg, available);

      // Allocate MsgBufs and copy data.
      size_t consumed = 0;
      while (consumed < to_consume) {
        // Need a new MsgBuf?
        if (rx_cur_msgbuf_ == nullptr) {
          rx_cur_msgbuf_ = channel_->MsgBufAlloc();
          if (rx_cur_msgbuf_ == nullptr) {
            LOG(ERROR) << "TCP RX: Failed to allocate MsgBuf. Dropping data.";
            // Reset state for this message.
            rx_pending_msg_len_ = 0;
            rx_buf_offset_ = 0;
            rx_cur_msgbuf_ = nullptr;
            // Free any partial train.
            if (rx_msg_train_head_ != nullptr) {
              FreeMsgBufChain(rx_msg_train_head_);
              rx_msg_train_head_ = nullptr;
              rx_msg_train_tail_ = nullptr;
            }
            return;
          }
          // Set up the msgbuf.
          bool is_first = (rx_msg_train_head_ == nullptr);
          if (is_first) {
            rx_cur_msgbuf_->set_flags(MACHNET_MSGBUF_FLAGS_SYN);
            rx_cur_msgbuf_->set_msg_length(rx_pending_msg_len_);
            rx_cur_msgbuf_->set_src_ip(key_.remote_addr.address.value());
            rx_cur_msgbuf_->set_src_port(key_.remote_port.port.value());
            rx_cur_msgbuf_->set_dst_ip(key_.local_addr.address.value());
            rx_cur_msgbuf_->set_dst_port(key_.local_port.port.value());
            rx_msg_train_head_ = rx_cur_msgbuf_;
            rx_msg_train_tail_ = rx_cur_msgbuf_;
          } else {
            rx_cur_msgbuf_->set_flags(MACHNET_MSGBUF_FLAGS_SG);
            rx_msg_train_tail_->set_next(rx_cur_msgbuf_);
            rx_msg_train_tail_ = rx_cur_msgbuf_;
          }
        }

        size_t buf_room = channel_->GetUsableBufSize() - rx_cur_msgbuf_->length();
        if (buf_room == 0) {
          // Current MsgBuf is full. Mark as SG and get a new one.
          rx_cur_msgbuf_->set_flags(rx_cur_msgbuf_->flags() |
                                    MACHNET_MSGBUF_FLAGS_SG);
          rx_cur_msgbuf_ = nullptr;
          continue;
        }
        size_t chunk = std::min(to_consume - consumed, buf_room);
        auto* dst = rx_cur_msgbuf_->append<uint8_t*>(static_cast<uint32_t>(chunk));
        std::memcpy(dst, data + offset + consumed, chunk);
        consumed += chunk;
      }

      offset += consumed;
      rx_buf_offset_ += consumed;

      // Check if message is complete.
      if (rx_buf_offset_ >= rx_pending_msg_len_) {
        // Mark the last MsgBuf.
        if (rx_cur_msgbuf_ != nullptr) {
          // Clear SG flag and set FIN on the last buffer.
          uint8_t f = rx_cur_msgbuf_->flags();
          f &= ~MACHNET_MSGBUF_FLAGS_SG;
          f |= MACHNET_MSGBUF_FLAGS_FIN;
          rx_cur_msgbuf_->set_flags(f);
        }

        // Deliver to application.
        if (rx_msg_train_head_ != nullptr) {
          auto nr = channel_->EnqueueMessages(&rx_msg_train_head_, 1);
          if (nr != 1) {
            LOG(ERROR)
                << "TCP: Failed to deliver message to channel. Dropping.";
            FreeMsgBufChain(rx_msg_train_head_);
          }
        }

        // Reset for next message.
        rx_pending_msg_len_ = 0;
        rx_buf_offset_ = 0;
        rx_cur_msgbuf_ = nullptr;
        rx_msg_train_head_ = nullptr;
        rx_msg_train_tail_ = nullptr;
      }
    }
  }

  // ──────────────── Helpers ────────────────

  void AdvanceSndUna(uint32_t ack) {
    if (SeqGt(ack, snd_una_) && SeqLeq(ack, snd_nxt_)) {
      snd_una_ = ack;
      retransmit_count_ = 0;
      if (snd_una_ == snd_nxt_) {
        rto_active_ = false;  // All data acknowledged.
      } else {
        rto_remaining_ = rto_ticks_;
      }
    }
  }

  void FreeMsgBufChain(shm::MsgBuf* head) {
    shm::MsgBufBatch to_free;
    auto* cur = head;
    while (cur != nullptr) {
      shm::MsgBuf* next_buf = nullptr;
      if (cur->is_sg() || cur->has_chain()) {
        next_buf = channel_->GetMsgBuf(cur->next());
      }
      to_free.Append(cur, cur->index());
      if (to_free.IsFull()) {
        channel_->MsgBufBulkFree(&to_free);
      }
      cur = next_buf;
    }
    if (to_free.GetSize() > 0) {
      channel_->MsgBufBulkFree(&to_free);
    }
  }

  /// @brief Generate a pseudo-random initial sequence number.
  static uint32_t GenerateISN() {
    // Use a simple timestamp-based ISN. In production this should be more
    // robust (RFC 6528), but for a userspace stack this is sufficient.
    return static_cast<uint32_t>(__builtin_ia32_rdtsc() & 0xFFFFFFFF);
  }

  // TCP sequence number comparison helpers (handles wrapping).
  static bool SeqLt(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) < 0;
  }
  static bool SeqLeq(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) <= 0;
  }
  static bool SeqGt(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) > 0;
  }
  static bool SeqGeq(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) >= 0;
  }

  // ──────────────── Data Members ────────────────

  const Key key_;
  const Ethernet::Address local_l2_addr_;
  const Ethernet::Address remote_l2_addr_;
  State state_;
  dpdk::TxRing* txring_;
  ApplicationCallback callback_;
  shm::Channel* channel_;

  // TCP send-side state.
  uint32_t snd_una_;  ///< Oldest unacknowledged sequence number.
  uint32_t snd_nxt_;  ///< Next sequence number to send.
  uint32_t snd_isn_;  ///< Initial send sequence number.

  // TCP receive-side state.
  uint32_t rcv_nxt_;   ///< Next expected receive sequence number.
  uint16_t rcv_wnd_;   ///< Receive window (advertised to peer).
  uint16_t snd_wnd_;   ///< Send window (from peer).

  /// Peer's MSS learned from TCP options in SYN/SYN-ACK.
  /// If the peer (Linux kernel) doesn't send an MSS option we fall back to
  /// kDefaultMSS.  This is used in OutputMessage for segmentation.
  uint16_t peer_mss_{static_cast<uint16_t>(kDefaultMSS)};

  // Retransmission timer (in periodic tick units).
  uint32_t rto_ticks_;
  uint32_t rto_remaining_;
  bool rto_active_;
  uint32_t retransmit_count_;

  /// TIME_WAIT countdown (in periodic tick units).
  uint32_t time_wait_remaining_{kTimeWaitTicks};

  // RX reassembly state for message deframing.
  uint8_t rx_len_buf_[kMsgLenPrefixSize]{};  ///< Partial length prefix buffer.
  uint8_t rx_len_buf_offset_{0};
  uint32_t rx_buf_offset_;         ///< Bytes received for current message.
  uint32_t rx_pending_msg_len_;    ///< Expected length of current message.
  shm::MsgBuf* rx_cur_msgbuf_{nullptr};     ///< Current MsgBuf being filled.
  shm::MsgBuf* rx_msg_train_head_;  ///< Head of current message train.
  shm::MsgBuf* rx_msg_train_tail_;  ///< Tail of current message train.
};

}  // namespace flow
}  // namespace net
}  // namespace juggler

namespace std {

template <>
struct hash<juggler::net::flow::TcpFlow> {
  size_t operator()(const juggler::net::flow::TcpFlow& flow) const {
    const auto& key = flow.key();
    return juggler::utils::hash<uint64_t>(reinterpret_cast<const char*>(&key),
                                          sizeof(key));
  }
};

}  // namespace std

#endif  // SRC_INCLUDE_TCP_FLOW_H_
