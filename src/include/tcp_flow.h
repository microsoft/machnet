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
#include <rte_ip.h>
#include <tcp.h>
#include <types.h>
#include <utils.h>

#include <algorithm>
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
      // Queue the FIN behind any buffered data.  PumpSend emits it once the
      // data has drained and keeps the RTO armed so a lost FIN retransmits.
      fin_pending_ = true;
      state_ = (state_ == State::kEstablished) ? State::kFinWait1
                                                : State::kLastAck;
      PumpSend();
    } else {
      SendRst();
      state_ = State::kClosed;
      rto_active_ = false;
    }
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

    // A received ACK may have advanced snd_una_ and/or opened the peer's
    // receive window, and RX-side transitions may now permit TX.  Push out any
    // buffered data (and a pending FIN) that the window now allows.
    PumpSend();
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
      FreeMsgBufChain(msg);  // Never leak the chain on an early return.
      return;
    }

    const uint32_t msg_len = msg->msg_length();
    const uint32_t framed_len = kMsgLenPrefixSize + msg_len;

    VLOG(1) << "TCP OutputMessage: " << key_.ToString()
            << " msg_len=" << msg_len << " snd_nxt=" << snd_nxt_
            << " snd_una=" << snd_una_ << " buffered=" << snd_buf_.size();

    // Bound the send buffer.  Under a stalled peer/app this applies
    // backpressure instead of growing without limit.
    if (snd_buf_.size() + framed_len > kMaxSendBuf) {
      LOG(ERROR) << "TCP send buffer full (" << snd_buf_.size()
                 << " bytes); dropping " << msg_len << "-byte message on "
                 << key_.ToString();
      FreeMsgBufChain(msg);
      return;
    }

    // Frame the message into the byte stream: 4-byte big-endian length prefix
    // followed by the payload.  We append into snd_buf_ rather than sending
    // immediately, so the bytes remain available for window-gated transmission
    // and retransmission.
    const size_t buf_before = snd_buf_.size();
    uint32_t net_len = htobe32(msg_len);
    const uint8_t* lp = reinterpret_cast<const uint8_t*>(&net_len);
    snd_buf_.insert(snd_buf_.end(), lp, lp + kMsgLenPrefixSize);

    // Copy the payload from the MsgBuf chain, validating the declared length
    // against the bytes actually present so a bad msg_length cannot desync the
    // stream or spin the segmentation loop.
    uint32_t copied = 0;
    auto* cur = msg;
    while (cur != nullptr && copied < msg_len) {
      const auto* src = static_cast<const uint8_t*>(cur->head_data());
      const uint32_t n =
          std::min<uint32_t>(cur->length(), msg_len - copied);
      snd_buf_.insert(snd_buf_.end(), src, src + n);
      copied += n;
      if (cur->is_sg() || cur->has_chain()) {
        cur = channel_->GetMsgBuf(cur->next());
      } else {
        cur = nullptr;
      }
    }

    if (copied != msg_len) {
      LOG(ERROR) << "TCP: msg_length=" << msg_len << " but chain holds "
                 << copied << " bytes; dropping message on " << key_.ToString();
      snd_buf_.resize(buf_before);  // Roll back this partial frame.
      FreeMsgBufChain(msg);
      return;
    }

    // The bytes now live in snd_buf_; release the source chain.
    FreeMsgBufChain(msg);

    // Transmit whatever the peer window currently allows.
    PumpSend();
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
      // Notify the application on teardown from ANY state (not just SYN_SENT),
      // so an established connection that dies does not fail silently.
      callback_(channel_, false, key_);
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
      case State::kEstablished:
      case State::kCloseWait:
      case State::kFinWait1:
      case State::kLastAck:
        if (snd_wnd_ == 0 && SeqLt(snd_nxt_, SndBufferedEndSeq())) {
          // Peer window is closed but we have data to send: probe it so a lost
          // window-update ACK cannot deadlock the connection.
          LOG(INFO) << "TCP zero-window probe on " << key_.ToString();
          SendZeroWindowProbe();
        } else {
          // Go-back-N: rewind to the oldest unacked byte and retransmit the
          // buffered data (and the FIN) from there.
          LOG(INFO) << "TCP RTO retransmit from snd_una=" << snd_una_
                    << " snd_nxt=" << snd_nxt_ << " on " << key_.ToString();
          RetransmitUnacked();
        }
        break;
      default:
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

  /// Seed the TCP checksum field with the IPv4 pseudo-header partial checksum.
  /// The DPDK TX offload contract for RTE_MBUF_F_TX_TCP_CKSUM (non-TSO) is:
  /// software pre-loads the pseudo-header sum here, hardware completes it over
  /// the TCP header + payload.  Leaving it 0 (valid for UDP, where checksums
  /// are optional) yields an invalid TCP checksum that the peer drops.  Must run
  /// after PrepareL3Header (IP total_length final) and offload_tcpv4_csum().
  /// @note tcph->checksum is a raw uint16_t holding a network-order partial
  ///       checksum — do NOT wrap it in be16_t, which would byte-swap it.
  void FinalizeTcpChecksum(dpdk::Packet* packet) const {
    auto* ipv4h = packet->head_data<struct rte_ipv4_hdr*>(sizeof(Ethernet));
    auto* tcph = packet->head_data<Tcp*>(sizeof(Ethernet) + sizeof(Ipv4));
    tcph->checksum = rte_ipv4_phdr_cksum(ipv4h, /*ol_flags=*/0);
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
    FinalizeTcpChecksum(packet);

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
    FinalizeTcpChecksum(packet);
    txring_->SendPackets(&packet, 1);
  }

  void SendSyn() {
    // Always send the SYN from the fixed ISN and set snd_nxt_ absolutely, so a
    // retransmitted SYN (from PeriodicCheck) reuses the same sequence number
    // instead of consuming a fresh one.  Incrementing on every call drifted
    // snd_nxt_ past the peer's ack after the first retransmit, permanently
    // breaking the handshake.  Mirrors SendSynAck, which is already idempotent.
    SendControlPacketWithMSS(snd_isn_, 0, Tcp::kSyn,
                              static_cast<uint16_t>(kDefaultMSS));
    snd_nxt_ = snd_isn_ + 1;  // SYN consumes one sequence number.
  }

  void SendSynAck() {
    SendControlPacketWithMSS(snd_isn_, rcv_nxt_, Tcp::kSyn | Tcp::kAck,
                              static_cast<uint16_t>(kDefaultMSS));
    snd_nxt_ = snd_isn_ + 1;  // SYN-ACK consumes one sequence number.
  }

  void SendAck() { SendControlPacket(snd_nxt_, rcv_nxt_, Tcp::kAck); }

  void SendRst() { SendControlPacket(snd_nxt_, 0, Tcp::kRst); }

  // ──────────────── Helpers: Window-gated Data TX ────────────────

  /// Sequence number one past the last buffered send byte.
  uint32_t SndBufferedEndSeq() const {
    return snd_una_ + static_cast<uint32_t>(snd_buf_.size());
  }

  /// Bytes transmitted but not yet acknowledged.
  uint32_t BytesInFlight() const { return snd_nxt_ - snd_una_; }

  /// True while a transmitted FIN has not yet been acknowledged.
  bool FinOutstanding() const {
    return fin_sent_ && SeqLt(snd_una_, snd_fin_seq_ + 1);
  }

  /// Effective per-segment payload: honor the peer's MSS but never exceed our
  /// conservative default (also bounds the segment to the mbuf data room,
  /// avoiding an allocation failure on an over-large advertised MSS).
  uint16_t EffectiveMSS() const {
    return std::min<uint16_t>(peer_mss_, static_cast<uint16_t>(kDefaultMSS));
  }

  /// Build and transmit a single data segment carrying `len` bytes of the send
  /// buffer starting at sequence `seq`.  Returns false (without side effects on
  /// sequence state) if a packet could not be allocated, so the caller can
  /// retry later instead of crashing or emitting a short frame.
  bool SendDataSegment(uint32_t seq, uint32_t len) {
    DCHECK(SeqGeq(seq, snd_una_));
    const size_t off = seq - snd_una_;
    DCHECK_LE(off + len, snd_buf_.size());

    auto* packet = txring_->GetPacketPool()->PacketAlloc();
    if (packet == nullptr) [[unlikely]] {
      LOG(ERROR) << "TCP: packet pool exhausted; deferring TX on "
                 << key_.ToString();
      return false;
    }
    dpdk::Packet::Reset(packet);

    const size_t hdr_len = sizeof(Ethernet) + sizeof(Ipv4) + sizeof(Tcp);
    CHECK_NOTNULL(packet->append(static_cast<uint16_t>(hdr_len + len)));

    uint8_t* dst =
        packet->head_data<uint8_t*>(static_cast<uint16_t>(hdr_len));
    std::copy_n(snd_buf_.begin() + off, len, dst);

    PrepareL2Header(packet);
    PrepareL3Header(packet);
    PrepareL4Header(packet, seq, rcv_nxt_, Tcp::kAck | Tcp::kPsh);
    packet->offload_tcpv4_csum();
    FinalizeTcpChecksum(packet);
    txring_->SendPackets(&packet, 1);
    return true;
  }

  /// Transmit a FIN (carrying the current ACK) at snd_nxt_.  Returns false on
  /// allocation failure so the FIN is retried rather than skipped.
  bool SendFinSegment() {
    auto* packet = txring_->GetPacketPool()->PacketAlloc();
    if (packet == nullptr) [[unlikely]] {
      LOG(ERROR) << "TCP: packet pool exhausted; deferring FIN on "
                 << key_.ToString();
      return false;
    }
    dpdk::Packet::Reset(packet);
    const size_t pkt_len = sizeof(Ethernet) + sizeof(Ipv4) + sizeof(Tcp);
    CHECK_NOTNULL(packet->append(static_cast<uint16_t>(pkt_len)));
    PrepareL2Header(packet);
    PrepareL3Header(packet);
    PrepareL4Header(packet, snd_nxt_, rcv_nxt_, Tcp::kFin | Tcp::kAck);
    packet->offload_tcpv4_csum();
    FinalizeTcpChecksum(packet);
    txring_->SendPackets(&packet, 1);
    return true;
  }

  /// Push out as much buffered data as the peer's receive window and MSS allow,
  /// starting at snd_nxt_, then emit a pending FIN once all data has drained.
  /// Safe to call repeatedly (from OutputMessage, on every received ACK, and
  /// after RX-side state transitions); it only ever advances snd_nxt_ within
  /// the window, so a closed window simply sends nothing.
  void PumpSend() {
    if (state_ != State::kEstablished && state_ != State::kCloseWait &&
        state_ != State::kFinWait1 && state_ != State::kLastAck) {
      return;  // Data may only flow in these states.
    }

    const uint32_t data_end = SndBufferedEndSeq();
    const uint32_t win_edge = snd_una_ + snd_wnd_;  // Peer's advertised edge.
    const uint16_t eff_mss = EffectiveMSS();

    // Send data segments up to min(buffered end, window edge).
    while (SeqLt(snd_nxt_, data_end) && SeqLt(snd_nxt_, win_edge)) {
      const uint32_t to_win = win_edge - snd_nxt_;
      const uint32_t to_end = data_end - snd_nxt_;
      const uint32_t seg_len =
          std::min<uint32_t>(std::min(to_win, to_end), eff_mss);
      if (!SendDataSegment(snd_nxt_, seg_len)) break;  // Pool empty; retry later.
      snd_nxt_ += seg_len;
    }

    // Emit the FIN only after every buffered data byte has been transmitted, so
    // it carries the correct (highest) sequence number.
    if (fin_pending_ && !fin_sent_ && snd_nxt_ == data_end) {
      if (SendFinSegment()) {
        snd_fin_seq_ = snd_nxt_;
        snd_nxt_++;  // FIN consumes one sequence number.
        fin_sent_ = true;
      }
    }

    // Keep the RTO/persist timer armed whenever anything is outstanding or
    // still waiting to be sent (data blocked by a closed window needs the
    // persist timer to fire).
    const bool pending = BytesInFlight() > 0 || FinOutstanding() ||
                          SeqLt(snd_nxt_, data_end);
    if (pending && !rto_active_) {
      rto_active_ = true;
      rto_remaining_ = rto_ticks_;
    }
  }

  /// Go-back-N retransmission: rewind snd_nxt_ to the oldest unacked byte and
  /// resend the buffered data (and a re-sent FIN) from there.
  void RetransmitUnacked() {
    snd_nxt_ = snd_una_;
    fin_sent_ = false;  // Re-sent after the retransmitted data.
    PumpSend();
  }

  /// Zero-window probe: send a single byte at snd_nxt_ (without advancing it)
  /// to force the peer to re-advertise its window.
  void SendZeroWindowProbe() {
    if (SeqLt(snd_nxt_, SndBufferedEndSeq())) {
      SendDataSegment(snd_nxt_, 1);
    }
  }

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

    // Our FIN is "acked" only once it has actually been transmitted (it may
    // still be deferred behind window-limited data) and its sequence number
    // has been acknowledged.
    bool our_fin_acked = fin_sent_ && !FinOutstanding();

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
      const uint32_t acked = ack - snd_una_;
      // Drop acknowledged octets from the front of the send buffer.  Capped at
      // the buffer size so a FIN's sequence number (which occupies no buffer
      // byte) and direct-state unit tests cannot over-erase.
      const size_t drop =
          std::min<size_t>(acked, snd_buf_.size());
      snd_buf_.erase(snd_buf_.begin(), snd_buf_.begin() + drop);
      snd_una_ = ack;
      retransmit_count_ = 0;

      // The timer stays armed while any data or the FIN remains unacknowledged;
      // otherwise everything is delivered and it can be disarmed.
      if (snd_una_ == snd_nxt_ && snd_buf_.empty() && !FinOutstanding()) {
        rto_active_ = false;
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

  /// Retransmission / send buffer. Holds the outgoing TCP byte stream from
  /// snd_una_ onward: snd_buf_[0] is the octet with sequence number snd_una_,
  /// and the buffer spans [snd_una_, snd_una_ + snd_buf_.size()).  Bytes are
  /// appended by OutputMessage (framed length prefix + payload), transmitted
  /// by PumpSend as the peer window allows, retransmitted from snd_una_ on
  /// RTO, and dropped from the front as ACKs advance snd_una_.  This is the
  /// state that makes lost data recoverable.
  std::deque<uint8_t> snd_buf_;

  /// Upper bound on buffered-but-unacked send data.  When the peer/app stalls
  /// this caps memory instead of growing unbounded; further sends are dropped
  /// with an error rather than OOMing the engine.  Sized to two max-length
  /// messages so a single MACHNET_MSG_MAX_LEN message is never rejected for
  /// being large — only genuine accumulation triggers backpressure.
  static constexpr size_t kMaxSendBuf = 2 * MACHNET_MSG_MAX_LEN;

  /// FIN bookkeeping.  A close request sets fin_pending_; the FIN octet is put
  /// on the wire (at snd_fin_seq_, consuming one sequence number) only once all
  /// buffered data has been transmitted, so its sequence number is correct even
  /// when the app closes with data still queued behind a closed window.
  bool fin_pending_{false};
  bool fin_sent_{false};
  uint32_t snd_fin_seq_{0};

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
