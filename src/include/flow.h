/**
 * @file flow.h
 * @brief Class to abstract the components and functionality of a single flow.
 */

#ifndef SRC_INCLUDE_FLOW_H_
#define SRC_INCLUDE_FLOW_H_

#include <cc.h>
#include <channel.h>
#include <channel_msgbuf.h>
#include <common.h>
#include <dpdk.h>
#include <ether.h>
#include <flow_key.h>
#include <glog/logging.h>
#include <ipv4.h>
#include <machnet_common.h>
#include <machnet_pkthdr.h>
#include <packet.h>
#include <packet_pool.h>
#include <pmd.h>
#include <types.h>
#include <udp.h>
#include <utils.h>

#include <cstdint>
#include <optional>
#include <queue>
#include <unordered_map>

namespace juggler {
namespace net {
namespace flow {

class TXTracking {
 public:
  TXTracking() = delete;
  explicit TXTracking(shm::Channel* channel)
      : channel_(CHECK_NOTNULL(channel)),
        oldest_unacked_msgbuf_(nullptr),
        oldest_unsent_msgbuf_(nullptr),
        last_msgbuf_(nullptr),
        num_unsent_msgbufs_(0),
        num_tracked_msgbufs_(0) {}

  const uint32_t NumUnsentMsgbufs() const { return num_unsent_msgbufs_; }
  shm::MsgBuf* GetOldestUnackedMsgBuf() const { return oldest_unacked_msgbuf_; }

  void ReceiveAcks(uint32_t num_acked_pkts) {
    shm::MsgBufBatch to_free;
    while (num_acked_pkts) {
      auto msgbuf = oldest_unacked_msgbuf_;
      DCHECK(msgbuf != nullptr);
      if (msgbuf != last_msgbuf_) {
        DCHECK_NE(oldest_unacked_msgbuf_, oldest_unsent_msgbuf_)
            << "Releasing an unsent msgbuf!";
        oldest_unacked_msgbuf_ = channel_->GetMsgBuf(msgbuf->next());
      } else {
        oldest_unacked_msgbuf_ = nullptr;
        last_msgbuf_ = nullptr;
      }
      to_free.Append(msgbuf, msgbuf->index());
      if (to_free.IsFull()) {
        num_tracked_msgbufs_ -= to_free.GetSize();
        CHECK(channel_->MsgBufBulkFree(&to_free));
      }
      num_acked_pkts--;
    }

    num_tracked_msgbufs_ -= to_free.GetSize();
    CHECK(channel_->MsgBufBulkFree(&to_free));
  }

  void Append(shm::MsgBuf* msgbuf) {
    DCHECK(msgbuf->is_first());
    // Append the message at the end of the chain of buffers, if any.
    if (last_msgbuf_ == nullptr) {
      // This is the first pending message buffer in the flow.
      DCHECK(oldest_unsent_msgbuf_ == nullptr);
      last_msgbuf_ = channel_->GetMsgBuf(msgbuf->last());
      oldest_unsent_msgbuf_ = msgbuf;
      oldest_unacked_msgbuf_ = msgbuf;
    } else {
      // This is not the first message buffer in the flow.
      DCHECK(oldest_unacked_msgbuf_ != nullptr);
      // Let's enqueue the new message buffer at the end of the chain.
      last_msgbuf_->link(msgbuf);
      DCHECK(!(last_msgbuf_->is_last() && last_msgbuf_->is_sg()));
      // Update the last buffer pointer to point to the current buffer.
      last_msgbuf_ = channel_->GetMsgBuf(msgbuf->last());
      if (oldest_unsent_msgbuf_ == nullptr) oldest_unsent_msgbuf_ = msgbuf;
    }

    const auto msg_length = msgbuf->msg_length();
    const auto effective_buffer_size = channel_->GetUsableBufSize();
    const auto msg_buffers_nr =
        (msg_length + effective_buffer_size - 1) / effective_buffer_size;
    num_unsent_msgbufs_ += msg_buffers_nr;
    num_tracked_msgbufs_ += msg_buffers_nr;
  }

  std::optional<shm::MsgBuf*> GetAndUpdateOldestUnsent() {
    if (oldest_unsent_msgbuf_ == nullptr) {
      DCHECK_EQ(NumUnsentMsgbufs(), 0);
      return std::nullopt;
    }

    auto msgbuf = oldest_unsent_msgbuf_;
    if (oldest_unsent_msgbuf_ != last_msgbuf_) {
      oldest_unsent_msgbuf_ =
          channel_->GetMsgBuf(oldest_unsent_msgbuf_->next());
    } else {
      oldest_unsent_msgbuf_ = nullptr;
    }

    num_unsent_msgbufs_--;
    return msgbuf;
  }

 private:
  const uint32_t NumTrackedMsgbufs() const { return num_tracked_msgbufs_; }
  const shm::MsgBuf* GetLastMsgBuf() const { return last_msgbuf_; }
  const shm::MsgBuf* GetOldestUnsentMsgBuf() const {
    return oldest_unsent_msgbuf_;
  }

  shm::Channel* channel_;

  /*
   * For the linked list of shm::MsgBufs in the channel (chain going downwards),
   * we track 3 pointers
   *
   * B   -> oldest sent but unacknowledged MsgBuf
   * ...
   * B   -> oldest unsent MsgBuf
   * ...
   * B   -> last MsgBuf, among all active messages in this flow
   */

  shm::MsgBuf* oldest_unacked_msgbuf_;
  shm::MsgBuf* oldest_unsent_msgbuf_;
  shm::MsgBuf* last_msgbuf_;

  uint32_t num_unsent_msgbufs_;
  uint32_t num_tracked_msgbufs_;
};

/**
 * @class RXTracking
 * @brief Tracking for message buffers that are received from the network. This
 * class is handling out-of-order reception of packets, and delivers complete
 * messages to the application.
 */
class RXTracking {
 public:
  using MachnetPktHdr = net::MachnetPktHdr;

  static constexpr std::size_t kReassemblyQueueDefaultSize = 64;
  static constexpr std::size_t kReassemblyQueueDefaultSizeMask =
      (kReassemblyQueueDefaultSize - 1);
  static_assert(utils::is_power_of_two(kReassemblyQueueDefaultSize),
                "ReassemblyQueue size must be a power of two.");
  RXTracking(const RXTracking&) = delete;
  RXTracking(uint32_t local_ip, uint16_t local_port, uint32_t remote_ip,
             uint16_t remote_port, shm::Channel* channel)
      : local_ip_(local_ip),
        local_port_(local_port),
        remote_ip_(remote_ip),
        remote_port_(remote_port),
        channel_(CHECK_NOTNULL(channel)),
        reass_q_head_(0),
        reass_q_tail_(0),
        reass_q_(kReassemblyQueueDefaultSize, nullptr),
        oldest_inorder_item_(nullptr),
        latest_inorder_item_(nullptr) {}

  /**
   * @brief Return the size of the reassembly queue.
   * @return The size of the reassembly queue.
   */
  const size_t ReassemblyQueueCapacity() const { return reass_q_.size() - 1; }

  /**
   * @brief Check if the reassembly queue is empty. If empty, it means that
   * there are no out-of-order packets delivered and pending for reassembly.
   * Buffering of in-order packets still happens without occupying space in the
   * reassembly queue; we deliver the in-order packets to the application when a
   * full message is received.
   * @return True if the reassembly queue is empty, false otherwise.
   */
  bool IsReassemblyQueueEmpty() const {
    return ((reass_q_head_ - reass_q_tail_) &
            kReassemblyQueueDefaultSizeMask) == 0;
  }

  void Push(swift::Pcb* pcb, const dpdk::Packet* packet) {
    const size_t net_hdr_len = sizeof(Ethernet) + sizeof(Ipv4) + sizeof(Udp);
    const auto* machneth = packet->head_data<MachnetPktHdr*>(net_hdr_len);
    const auto* payload =
        packet->head_data<uint8_t*>(net_hdr_len + sizeof(MachnetPktHdr));
    const auto seqno = machneth->seqno.value();
    const auto expected_seqno = pcb->rcv_nxt;

    if (swift::seqno_lt(seqno, expected_seqno)) [[unlikely]] {  // NOLINT
      // Packet is a duplicate.
      // LOG(INFO) << "Received old packet: " << seqno << " < " <<
      // expected_seqno;
      return;
    }  // NOLINT

    auto distance = seqno - expected_seqno;
    if (distance >= ReassemblyQueueCapacity()) [[unlikely]] {  // NOLINT
      // Packet is too far ahead; a packet train was dropped?
      // We cannot buffer this packet's data.
      LOG(ERROR) << "Packet too far ahead, seqno: " << seqno
                 << ", expected: " << expected_seqno;
      return;
    }  // NOLINT

    // Check if we need to expand the reassembly queue window.
    auto reass_q_window =
        (reass_q_head_ - reass_q_tail_) & kReassemblyQueueDefaultSizeMask;
    if (distance >= reass_q_window) [[unlikely]] {  // NOLINT
      reass_q_head_ =
          (reass_q_tail_ + distance + 1) & kReassemblyQueueDefaultSizeMask;
    }  // NOLINT

    // Packet is in-order or ahead of the next expected sequence number.
    auto pos = (reass_q_tail_ + distance) & kReassemblyQueueDefaultSizeMask;
    // clang-format off
    if (reass_q_[pos] != nullptr) [[unlikely]] {  // NOLINT
        // Packet is a duplicate.
        return;
    }
    // clang-format on

    // Buffer the packet.
    // Copy the payload to a `MsgBuf` object, and set the flags
    // appropriately.
    auto* msgbuf = channel_->MsgBufAlloc();
    CHECK_NOTNULL(msgbuf);
    const size_t payload_len =
        packet->length() - net_hdr_len - sizeof(MachnetPktHdr);
    auto* msg_data = msgbuf->append<uint8_t*>(payload_len);
    utils::Copy(CHECK_NOTNULL(msg_data), payload, msgbuf->length());
    msgbuf->set_flags(machneth->msg_flags);
    msgbuf->set_src_ip(remote_ip_);
    msgbuf->set_src_port(remote_port_);
    msgbuf->set_dst_ip(local_ip_);
    msgbuf->set_dst_port(local_port_);
    DCHECK(!(msgbuf->is_last() && msgbuf->is_sg()));
    reass_q_[pos] = msgbuf;

    // Update the SACK bitmap for the newly received packet.
    pcb->sack_bitmap |= (1ULL << distance);
    pcb->sack_bitmap_count++;

    TryDequeueMsgBufs(pcb);
  }

 private:
  void TryDequeueMsgBufs(swift::Pcb* pcb) {
    while (reass_q_tail_ != reass_q_head_ &&
           reass_q_[reass_q_tail_] != nullptr) {
      // We have a contiguous sequence of buffers, so we can return at least the
      // first one.
      auto* msgbuf = reass_q_[reass_q_tail_];
      if (latest_inorder_item_ == nullptr) {
        latest_inorder_item_ = msgbuf;
        oldest_inorder_item_ = latest_inorder_item_;
      } else {
        latest_inorder_item_->set_next(msgbuf);
        latest_inorder_item_ = msgbuf;
      }
      TryDeliverMessage();
      reass_q_[reass_q_tail_] = nullptr;
      reass_q_tail_ = (reass_q_tail_ + 1) & kReassemblyQueueDefaultSizeMask;
      pcb->advance_rcv_nxt();
      // Shift the SACK bitmap by 1.
      pcb->sack_bitmap >>= 1;
      pcb->sack_bitmap_count--;
    }
  }

  void TryDeliverMessage() {
    DCHECK_NE(oldest_inorder_item_, nullptr);
    DCHECK_NE(latest_inorder_item_, nullptr);
    DCHECK(oldest_inorder_item_->is_first());
    if (!latest_inorder_item_->is_last()) return;

    DCHECK(!latest_inorder_item_->is_sg());
    // We have a complete message. Let's deliver it to the application.
    auto* msgbuf = oldest_inorder_item_;
    auto nr_delivered = channel_->EnqueueMessages(&msgbuf, 1);
    if (nr_delivered != 1) {
      // The channel is full.
      return;
    }
    oldest_inorder_item_ = nullptr;
    latest_inorder_item_ = nullptr;
  }

  const uint32_t local_ip_;
  const uint16_t local_port_;
  const uint32_t remote_ip_;
  const uint16_t remote_port_;
  shm::Channel* channel_;
  size_t reass_q_head_;
  size_t reass_q_tail_;
  std::vector<shm::MsgBuf*> reass_q_;
  // The oldest received `MsgBuf' we have received, but not yet submitted to the
  // application. This is always the first buffer of a message (chain of
  // buffers).
  shm::MsgBuf* oldest_inorder_item_;
  // This is the latest in-order `MsgBuf' we have received. There might be
  // subsequent segments that we have already received, but they are
  // out-of-order in the reassembly queue.
  shm::MsgBuf* latest_inorder_item_;
};

/**
 * @class Flow A flow is a connection between a local and a remote endpoint.
 * @brief Class to abstract the components and functionality of a single flow.
 * A flow is a bidirectional connection between two hosts, uniquely identified
 * by the 5-tuple: {SrcIP, DstIP, SrcPort, DstPort, Protocol}, Protocol is
 * always UDP.
 *
 * A flow is always associated with a single `Channel' object which serves as
 * the communication interface with the application to which the flow belongs.
 *
 * On normal operation, a flow is:
 *    - Receiving network packets from the NIC, which then converts to messages
 *      and enqueues to the `Channel', so that they reach the application.
 *    - Receiving messages from the application (via the `Channel'), which then
 *      converts to network packets and sends them out to the remote recipient.
 */
class Flow {
 public:
  using Ethernet = net::Ethernet;
  using Ipv4 = net::Ipv4;
  using Udp = net::Udp;
  using MachnetPktHdr = net::MachnetPktHdr;
  using ApplicationCallback =
      std::function<void(shm::Channel*, bool, const Key&)>;

  enum class State {
    kClosed,
    kSynSent,
    kSynReceived,
    kEstablished,
  };

  static constexpr char const* StateToString(State state) {
    switch (state) {
      case State::kClosed:
        return "CLOSED";
      case State::kSynSent:
        return "SYN_SENT";
      case State::kSynReceived:
        return "SYN_RECEIVED";
      case State::kEstablished:
        return "ESTABLISHED";
      default:
        LOG(FATAL) << "Unknown state";
        return "UNKNOWN";
    }
  }

  /**
   * @brief Construct a new flow.
   *
   * @param local_addr Local IP address.
   * @param local_port Local UDP port.
   * @param remote_addr Remote IP address.
   * @param remote_port Remote UDP port.
   * @param local_l2_addr Local L2 address.
   * @param remote_l2_addr Remote L2 address.
   * @param txring TX ring to send packets to.
   * @param channel Shared memory channel this flow is associated with.
   */
  Flow(const Ipv4::Address& local_addr, const Udp::Port& local_port,
       const Ipv4::Address& remote_addr, const Udp::Port& remote_port,
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
        pcb_(),
        tx_tracking_(CHECK_NOTNULL(channel)),
        rx_tracking_(local_addr.address.value(), local_port.port.value(),
                     remote_addr.address.value(), remote_port.port.value(),
                     CHECK_NOTNULL(channel)) {
    CHECK_NOTNULL(txring_->GetPacketPool());
  }
  ~Flow() {}
  /**
   * @brief Operator to compare if two flows are equal.
   * @param other Other flow to compare to.
   * @return true if the flows are equal, false otherwise.
   */
  bool operator==(const Flow& other) const { return key_ == other.key(); }

  /**
   * @brief Get the flow key.
   */
  const Key& key() const { return key_; }

  /**
   * @brief Get the associated channel.
   */
  shm::Channel* channel() const { return channel_; }

  /**
   * @brief Get the current state of the flow.
   */
  State state() const { return state_; }

  std::string ToString() const {
    return utils::Format(
        "%s [%s] <-> [%s]\n\t\t\t%s\n\t\t\t[TX Queue] Pending "
        "MsgBufs: "
        "%u",
        key_.ToString().c_str(), StateToString(state_),
        channel_->GetName().c_str(), pcb_.ToString().c_str(),
        tx_tracking_.NumUnsentMsgbufs());
  }

  bool Match(const dpdk::Packet* packet) const {
    const auto* ih = packet->head_data<Ipv4*>(sizeof(Ethernet));
    const auto* udph = packet->head_data<Udp*>(sizeof(Ethernet) + sizeof(Ipv4));

    return (ih->src_addr == key_.remote_addr &&
            ih->dst_addr == key_.local_addr &&
            udph->src_port == key_.remote_port &&
            udph->dst_port == key_.local_port);
  }

  bool Match(const shm::MsgBuf* tx_msgbuf) const {
    const auto* flow_info = tx_msgbuf->flow();
    return (flow_info->src_ip == key_.local_addr.address.value() &&
            flow_info->dst_ip == key_.remote_addr.address.value() &&
            flow_info->src_port == key_.local_port.port.value() &&
            flow_info->dst_port == key_.remote_port.port.value());
  }

  void InitiateHandshake() {
    CHECK(state_ == State::kClosed);
    SendSyn(pcb_.get_snd_nxt());
    pcb_.rto_reset();
    state_ = State::kSynSent;
  }

  void ShutDown() {
    switch (state_) {
      case State::kClosed:
        break;
      case State::kSynSent:
        [[fallthrough]];
      case State::kSynReceived:
        [[fallthrough]];
      case State::kEstablished:
        pcb_.rto_disable();
        SendRst();
        state_ = State::kClosed;
        break;
      default:
        LOG(FATAL) << "Unknown state";
    }
  }

  /**
   * @brief Push the received packet onto the ingress queue of the flow.
   * Decrypts packet if required, stores the payload in the relevant channel
   * shared memory space, and if the message is ready for delivery notifies the
   * application.
   *
   * If this is a transport control packet (e.g., ACK) it only updates
   * transport-related parameters for the flow.
   *
   * @param packet Pointer to the allocated packet on the rx ring of the driver
   */
  void InputPacket(const dpdk::Packet* packet) {
    // Parse the Machnet header of the packet.
    const size_t net_hdr_len = sizeof(Ethernet) + sizeof(Ipv4) + sizeof(Udp);
    auto* machneth = packet->head_data<MachnetPktHdr*>(net_hdr_len);

    // Sanity check on the Machnet header.
    // clang-format off
    if (machneth->magic.value() != MachnetPktHdr::kMagic) [[unlikely]] { // NOLINT
        LOG(ERROR) << "Invalid Machnet header magic: " << machneth->magic;
        return;
    } // NOLINT
    // clang-format on

    switch (machneth->net_flags) {
      case MachnetPktHdr::MachnetFlags::kSyn:
        // SYN packet received. For this to be valid it has to be an already
        // established flow with this SYN being a retransmission.
        if (state_ != State::kSynReceived && state_ != State::kClosed) {
          LOG(ERROR) << "SYN packet received for flow in state: "
                     << static_cast<int>(state_);
          return;
        }

        if (state_ == State::kClosed) {
          // If the flow is in closed state, we need to send a SYN-ACK packetj
          // and mark the flow as established.
          pcb_.rcv_nxt = machneth->seqno.value();
          pcb_.advance_rcv_nxt();
          SendSynAck(pcb_.get_snd_nxt());
          state_ = State::kSynReceived;
        } else if (state_ == State::kSynReceived) {
          // If the flow is in SYN-RECEIVED state, our SYN-ACK packet was lost.
          // We need to retransmit it.
          SendSynAck(pcb_.snd_una);
        }
        break;
      case MachnetPktHdr::MachnetFlags::kSynAck:
        // SYN-ACK packet received. For this to be valid it has to be an already
        // established flow with this SYN-ACK being a retransmission.
        if (state_ != State::kSynSent && state_ != State::kEstablished) {
          LOG(ERROR) << "SYN-ACK packet received for flow in state: "
                     << static_cast<int>(state_);
          return;
        }

        if (machneth->ackno.value() != pcb_.snd_nxt) [[unlikely]] {  // NOLINT
          LOG(ERROR) << "SYN-ACK packet received with invalid ackno: "
                     << machneth->ackno << " snd_una: " << pcb_.snd_una
                     << " snd_nxt: " << pcb_.snd_nxt;
          return;
        }

        if (state_ == State::kSynSent) {
          pcb_.snd_una++;
          pcb_.rcv_nxt = machneth->seqno.value();
          pcb_.advance_rcv_nxt();
          pcb_.rto_maybe_reset();
          // Mark the flow as established.
          state_ = State::kEstablished;
          // Notify the application that the flow is established.
          callback_(channel(), true, key());
        }
        // Send an ACK packet.
        SendAck();
        break;
      case MachnetPktHdr::MachnetFlags::kRst: {
        const auto seqno = machneth->seqno.value();
        const auto expected_seqno = pcb_.rcv_nxt;
        if (swift::seqno_eq(seqno, expected_seqno)) {
          // If the RST packet is in sequence, we can reset the flow.
          state_ = State::kClosed;
        }
      } break;
      case MachnetPktHdr::MachnetFlags::kAck:
        // ACK packet, update the flow.
        // update_flow(machneth);
        process_ack(machneth);
        break;
      case MachnetPktHdr::MachnetFlags::kData:
        // clang-format off
        if (state_ != State::kEstablished) [[unlikely]] { // NOLINT
          // clang-format on
          LOG(ERROR) << "Data packet received for flow in state: "
                     << static_cast<int>(state_);
          return;
        }
        // Data packet, process the payload.
        rx_tracking_.Push(&pcb_, packet);
        SendAck();
        break;
    }
  }

  /**
   * @brief Push a Message from the application onto the egress queue of
   * the flow. Segments the message, and encrypts the packets, and adds all
   * packets onto the egress queue.
   * Caller is responsible for freeing the MsgBuf object.
   *
   * @param msg Pointer to the first message buffer on a train of buffers,
   * aggregating to a partial or a full Message.
   */
  void OutputMessage(shm::MsgBuf* msg) {
    tx_tracking_.Append(msg);

    // TODO(ilias): We first need to check whether the cwnd is < 1, so that we
    // fallback to rate-based CC.

    // Calculate the effective window (in # of packets) to check whether we can
    // send more packets.
    TransmitPackets();
  }

  /**
   * @brief Periodically checks the state of the flow and performs necessary
   * actions.
   *
   * This method is called periodically to check the state of the flow, update
   * the RTO timer, retransmit unacknowledged messages, and potentially remove
   * the flow or notify the application about the connection state.
   *
   * @return Returns true if the flow should continue to be checked
   * periodically, false if the flow should be removed or closed.
   */
  bool PeriodicCheck() {
    // CLOSED state is terminal; the engine might remove the flow.
    if (state_ == State::kClosed) return false;

    if (pcb_.rto_disabled()) return true;

    pcb_.rto_advance();
    if (pcb_.max_rexmits_reached()) {
      if (state_ == State::kSynSent) {
        // Notify the application that the flow has not been established.
        LOG(INFO) << "Flow " << this << " failed to establish";
        callback_(channel(), false, key());
      }
      // TODO(ilias): Send RST packet.

      // Indicate removal of the flow.
      return false;
    }

    if (pcb_.rto_expired()) {
      // Retransmit the oldest unacknowledged message buffer.
      RTORetransmit();
    }

    return true;
  }

 private:
  void PrepareL2Header(dpdk::Packet* packet) {
    auto* eh = packet->head_data<Ethernet*>();
    eh->src_addr = local_l2_addr_;
    eh->dst_addr = remote_l2_addr_;
    eh->eth_type = be16_t(Ethernet::kIpv4);
    packet->set_l2_len(sizeof(*eh));
  }

  void PrepareL3Header(dpdk::Packet* packet) {
    // Prepare the L3 header.
    auto* ipv4h = packet->head_data<Ipv4*>(sizeof(Ethernet));
    ipv4h->version_ihl = 0x45;
    ipv4h->type_of_service = 0;
    ipv4h->packet_id = be16_t(0x1513);
    ipv4h->fragment_offset = be16_t(0);
    ipv4h->time_to_live = 64;
    ipv4h->next_proto_id = Ipv4::Proto::kUdp;
    ipv4h->total_length = be16_t(packet->length() - sizeof(Ethernet));
    ipv4h->src_addr = key_.local_addr;
    ipv4h->dst_addr = key_.remote_addr;
    ipv4h->hdr_checksum = 0;
    packet->set_l3_len(sizeof(*ipv4h));
  }

  void PrepareL4Header(dpdk::Packet* packet) {
    // Prepare the L4 header.
    auto* udph = packet->head_data<Udp*>(sizeof(Ethernet) + sizeof(Ipv4));
    udph->src_port = key_.local_port;
    udph->dst_port = key_.remote_port;
    udph->len = be16_t(packet->length() - sizeof(Ethernet) - sizeof(Ipv4));
    udph->cksum = be16_t(0);
    packet->offload_udpv4_csum();
  }

  void PrepareMachnetHdr(dpdk::Packet* packet, uint32_t seqno,
                         const MachnetPktHdr::MachnetFlags& net_flags,
                         uint8_t msg_flags = 0) {
    auto* machneth = packet->head_data<MachnetPktHdr*>(
        sizeof(Ethernet) + sizeof(Ipv4) + sizeof(Udp));
    machneth->magic = be16_t(MachnetPktHdr::kMagic);
    machneth->net_flags = net_flags;
    machneth->msg_flags = msg_flags;
    machneth->seqno = be32_t(seqno);
    machneth->ackno = be32_t(pcb_.ackno());
    machneth->sack_bitmap = be64_t(pcb_.sack_bitmap);
    machneth->sack_bitmap_count = be16_t(pcb_.sack_bitmap_count);
    machneth->timestamp1 = be64_t(0);
  }

  void SendControlPacket(uint32_t seqno,
                         const MachnetPktHdr::MachnetFlags& flags) {
    auto* packet = CHECK_NOTNULL(txring_->GetPacketPool()->PacketAlloc());
    dpdk::Packet::Reset(packet);

    const size_t kControlPacketSize =
        sizeof(Ethernet) + sizeof(Ipv4) + sizeof(Udp) + sizeof(MachnetPktHdr);
    CHECK_NOTNULL(packet->append(kControlPacketSize));
    PrepareL2Header(packet);
    PrepareL3Header(packet);
    PrepareL4Header(packet);
    PrepareMachnetHdr(packet, seqno, flags);

    // Send the packet.
    txring_->SendPackets(&packet, 1);
  }

  void SendSyn(uint32_t seqno) {
    SendControlPacket(seqno, MachnetPktHdr::MachnetFlags::kSyn);
  }

  void SendSynAck(uint32_t seqno) {
    SendControlPacket(seqno, MachnetPktHdr::MachnetFlags::kSyn |
                                 MachnetPktHdr::MachnetFlags::kAck);
  }

  void SendAck() {
    SendControlPacket(pcb_.seqno(), MachnetPktHdr::MachnetFlags::kAck);
  }

  void SendRst() {
    SendControlPacket(pcb_.seqno(), MachnetPktHdr::MachnetFlags::kRst);
  }

  /**
   * @brief This helper method prepares a network packet that carries the data
   * of a particular `MachnetMsgBuf_t'.
   *
   * @tparam copy_mode Copy mode of the packet. Either kMemCopy or kZeroCopy.
   * @param buf Pointer to the message buffer to be sent.
   * @param packet Pointer to an allocated packet.
   * @param seqno Sequence number of the packet.
   */
  template <CopyMode copy_mode>
  void PrepareDataPacket(shm::MsgBuf* msg_buf, dpdk::Packet* packet,
                         uint32_t seqno) {
    DCHECK(!(msg_buf->is_last() && msg_buf->is_sg()));
    // Header length after before the payload.
    const size_t hdr_length =
        (sizeof(Ethernet) + sizeof(Ipv4) + sizeof(Udp) + sizeof(MachnetPktHdr));
    const uint32_t pkt_len = hdr_length + msg_buf->length();
    CHECK_LE(pkt_len - sizeof(Ethernet), dpdk::PmdRing::kDefaultFrameSize);

    if constexpr (copy_mode == CopyMode::kMemCopy) {
      // In this mode we memory copy the packet payload.

      // We reset the allocated packet here. This is because if `FAST_FREE'
      // offload is enabled, the DPDK driver will not free any EXT buffers
      // attached to the mbuf which could lead to problems.
      // For the zerocopy mode, we do not need to reset it because we do all the
      // necessary initialization in the `attach_extbuf` method.
      dpdk::Packet::Reset(packet);

      // Allocate packet space.
      CHECK_NOTNULL(packet->append(pkt_len));
    } else {
      // In this mode we zero-copy the packet payload, by attaching the message
      // buffer.

      // Move the message buffer into the packet.
      auto* buf_va = msg_buf->base();
      const auto buf_iova = msg_buf->iova();
      const auto buf_len = msg_buf->size();
      const auto buf_data_ofs = msg_buf->data_offset();
      const auto buf_data_len = msg_buf->length();

      packet->attach_extbuf(buf_va, buf_iova, buf_len, buf_data_ofs,
                            buf_data_len, channel_->GetMbufExtShinfo());
      CHECK_NOTNULL(packet->prepend(hdr_length));
    }

    // Prepare network headers.
    PrepareL2Header(packet);
    PrepareL3Header(packet);
    PrepareL4Header(packet);

    // Prepare the Machnet-specific header.
    auto* machneth = packet->head_data<MachnetPktHdr*>(
        sizeof(Ethernet) + sizeof(Ipv4) + sizeof(Udp));
    machneth->magic = be16_t(MachnetPktHdr::kMagic);
    machneth->net_flags = MachnetPktHdr::MachnetFlags::kData;
    machneth->ackno = be32_t(UINT32_MAX);
    machneth->msg_flags = msg_buf->flags();
    DCHECK(!(msg_buf->is_last() && msg_buf->is_sg()));

    // machneth->msg_id = be32_t(msg_id_);
    machneth->seqno = be32_t(seqno);
    machneth->timestamp1 = be64_t(0);

    if constexpr (copy_mode == CopyMode::kMemCopy) {
      // Copy the payload.
      auto* payload = reinterpret_cast<uint8_t*>(machneth + 1);
      utils::Copy(payload, msg_buf->head_data(), msg_buf->length());
    }
  }

  void FastRetransmit() {
    // Retransmit the oldest unacknowledged message buffer.
    auto* packet = CHECK_NOTNULL(txring_->GetPacketPool()->PacketAlloc());
    PrepareDataPacket<CopyMode::kMemCopy>(tx_tracking_.GetOldestUnackedMsgBuf(),
                                          packet, pcb_.snd_una);
    txring_->SendPackets(&packet, 1);
    pcb_.rto_reset();
    pcb_.fast_rexmits++;
    LOG(INFO) << "Fast retransmitting packet " << pcb_.snd_una;
  }

  void RTORetransmit() {
    if (state_ == State::kEstablished) {
      LOG(INFO) << "RTO retransmitting data packet " << pcb_.snd_una;
      auto* packet = CHECK_NOTNULL(txring_->GetPacketPool()->PacketAlloc());
      PrepareDataPacket<CopyMode::kMemCopy>(
          tx_tracking_.GetOldestUnackedMsgBuf(), packet, pcb_.snd_una);
      txring_->SendPackets(&packet, 1);
    } else if (state_ == State::kSynReceived) {
      SendSynAck(pcb_.snd_una);
    } else if (state_ == State::kSynSent) {
      LOG(INFO) << "RTO retransmitting SYN packet " << pcb_.snd_una;
      // Retransmit the SYN packet.
      SendSyn(pcb_.snd_una);
    }
    pcb_.rto_reset();
    pcb_.rto_rexmits++;
  }

  /**
   * @brief Helper function to transmit a number of packets from the queue of
   * pending TX data.
   */
  void TransmitPackets() {
    auto remaining_packets =
        std::min(pcb_.effective_wnd(), tx_tracking_.NumUnsentMsgbufs());
    if (remaining_packets == 0) return;

    do {
      // Allocate a packet batch.
      dpdk::PacketBatch batch;
      auto pkt_cnt =
          std::min(remaining_packets, static_cast<uint32_t>(batch.GetRoom()));
      if (!txring_->GetPacketPool()->PacketBulkAlloc(&batch, pkt_cnt)) {
        LOG(ERROR) << "Failed to allocate packet batch";
        return;
      }

      // Prepare the packets.
      for (uint16_t i = 0; i < batch.GetSize(); i++) {
        auto msg = tx_tracking_.GetAndUpdateOldestUnsent();
        if (!msg.has_value()) break;
        auto* msg_buf = msg.value();
        auto* packet = batch.pkts()[i];
        if (kShmZeroCopyEnabled) {
          PrepareDataPacket<CopyMode::kZeroCopy>(msg_buf, packet,
                                                pcb_.get_snd_nxt());
        } else {
          PrepareDataPacket<CopyMode::kMemCopy>(msg_buf, packet,
                                                pcb_.get_snd_nxt());
        }
      }

      // TX.
      txring_->SendPackets(&batch);
      remaining_packets -= pkt_cnt;
    } while (remaining_packets);

    if (pcb_.rto_disabled()) pcb_.rto_enable();
  }

  void process_ack(const MachnetPktHdr* machneth) {
    auto ackno = machneth->ackno.value();
    if (swift::seqno_lt(ackno, pcb_.snd_una)) {
      return;
    } else if (swift::seqno_eq(ackno, pcb_.snd_una)) {
      // Duplicate ACK.
      pcb_.duplicate_acks++;
      // Update the number of out-of-order acknowledgements.
      pcb_.snd_ooo_acks = machneth->sack_bitmap_count.value();

      if (pcb_.duplicate_acks < swift::Pcb::kRexmitThreshold) {
        // We have not reached the threshold yet, so we do not do anything.
      } else if (pcb_.duplicate_acks == swift::Pcb::kRexmitThreshold) {
        // Fast retransmit.
        FastRetransmit();
      } else {
        // We have already done the fast retransmit, so we are now in the
        // fast recovery phase. We need to send a new packet for every ACK we
        // get.
        auto sack_bitmap = machneth->sack_bitmap.value();
        auto sack_bitmap_count = machneth->sack_bitmap_count.value();

        // First we check the SACK bitmap to see if there are more undelivered
        // packets. In fast recovery mode we get after a fast retransmit, and
        // for every new ACKnowledgement we get, we send a new packet.
        // Up until we get the first new acknowledgement, for the next in-order
        // packet, the SACK bitmap will likely keep expanding.
        // In order to avoid retransmitting multiple times other missing packets
        // in the bitmap, we skip holes: we use the number of duplicate ACKs to
        // skip previous holes.
        auto* msgbuf = tx_tracking_.GetOldestUnackedMsgBuf();
        size_t holes_to_skip =
            pcb_.duplicate_acks - swift::Pcb::kRexmitThreshold;
        size_t index = 0;
        while (sack_bitmap_count) {
          if ((sack_bitmap & (1ULL << index)) == 0) {
            // We found a missing packet.
            // We skip holes in the SACK bitmap that have already been
            // retransmitted.
            if (holes_to_skip-- == 0) {
              auto seqno = pcb_.snd_una + index;
              auto* packet_pool = txring_->GetPacketPool();
              auto* packet = CHECK_NOTNULL(packet_pool->PacketAlloc());
              PrepareDataPacket<CopyMode::kMemCopy>(msgbuf, packet, seqno);
              txring_->SendPackets(&packet, 1);
              pcb_.rto_reset();
              return;
            }
          } else {
            sack_bitmap_count--;
          }
          index++;
          msgbuf = channel_->GetMsgBuf(msgbuf->next());
        }
        // There is no other missing segment to retransmit, so we could send new
        // packets.
      }
      // clang-format off
    } else if (swift::seqno_gt(ackno, pcb_.snd_nxt)) [[unlikely]] { // NOLINT
      // clang-format on
      LOG(ERROR) << "ACK received for untransmitted data.";
      // clang-format off
    } else [[likely]] { // NOLINT
      // clang-format on
      // This is a valid ACK, acknowledging new data.
      size_t num_acked_packets = ackno - pcb_.snd_una;
      if (state_ == State::kSynReceived) {
        state_ = State::kEstablished;
        num_acked_packets--;
      }
      tx_tracking_.ReceiveAcks(num_acked_packets);
      pcb_.snd_una = ackno;
      pcb_.duplicate_acks = 0;
      pcb_.snd_ooo_acks = 0;
      pcb_.rto_rexmits = 0;
      pcb_.rto_maybe_reset();
    }

    TransmitPackets();
  }

  const Key key_;
  // A flow is identified by the 5-tuple (Proto is always UDP).
  const Ethernet::Address local_l2_addr_;
  const Ethernet::Address remote_l2_addr_;
  // Flow state.
  State state_;
  // Pointer to the TX ring for the flow to send packets on.
  dpdk::TxRing* txring_;
  // Callback to be invoked when the flow is either established or closed.
  ApplicationCallback callback_;
  // Shared pointer to the channel attached to this flow.
  shm::Channel* channel_;
  // Swift CC protocol control block.
  swift::Pcb pcb_;
  TXTracking tx_tracking_;
  RXTracking rx_tracking_;
};

}  // namespace flow
}  // namespace net
}  // namespace juggler

namespace std {

template <>
struct hash<juggler::net::flow::Flow> {
  size_t operator()(const juggler::net::flow::Flow& flow) const {
    const auto& key = flow.key();
    return juggler::utils::hash<uint64_t>(reinterpret_cast<const char*>(&key),
                                          sizeof(key));
  }
};

}  // namespace std

#endif  // SRC_INCLUDE_FLOW_H_
