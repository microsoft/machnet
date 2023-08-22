/**
 * @file channel_msgbuf.h
 * @brief Contains the class definition with helpers for `MachnetMsgBuf_t'.
 */
#ifndef SRC_INCLUDE_CHANNEL_MSGBUF_H_
#define SRC_INCLUDE_CHANNEL_MSGBUF_H_

#include <glog/logging.h>
#include <ipv4.h>
#include <machnet_common.h>
#include <udp.h>
#include <utils.h>

namespace juggler {
namespace shm {

/**
 * @brief Class `MsgBuf' abstracts a mesage buffer used in Machnet shared memory
 * channels.
 */
class MsgBuf {
 public:
  MsgBuf() = delete;

  template <typename T = void *>
  const T head_data(uint32_t offset = 0) const {
    return reinterpret_cast<T>(
        __machnet_channel_buf_data_ofs(&msg_buf_, offset));
  }

  template <typename T = void *>
  T head_data(uint32_t offset = 0) {
    return const_cast<T>(
        static_cast<const MsgBuf &>(*this).head_data<T>(offset));
  }

  template <typename T = void *>
  const T base() const {
    return reinterpret_cast<T>(__machnet_channel_buf_base(&msg_buf_));
  }

  template <typename T = void *>
  T base() {
    return const_cast<T>(static_cast<const MsgBuf &>(*this).base<T>());
  }

  void AssertMagic() const {
    DCHECK_EQ(msg_buf_.magic, (MACHNET_MSGBUF_MAGIC)) << "Invalid magic!";
  }

  // Getters.
  uint64_t iova() const { return msg_buf_.iova; }
  // Usable size of the buffer (excluding reserved space for headers)
  uint32_t size() const { return __machnet_channel_buf_size(&msg_buf_); }
  // Return the index of this `MsgBuf' in the buffer pool.
  uint32_t index() const { return msg_buf_.index; }
  // Return the data offset in the `MsgBuf' where the payload starts.
  uint32_t data_offset() const { return msg_buf_.data_ofs; }
  // Get the length of the data payload in this `MsgBuf'.
  uint32_t length() const { return msg_buf_.data_len; }
  // Returns the available space for prepending in the buffer.
  uint32_t headroom() const {
    return __machnet_channel_buf_headroom(&msg_buf_);
  }
  // Returns the available space for appending in the buffer.
  uint32_t tailroom() const {
    return __machnet_channel_buf_tailroom(&msg_buf_);
  }
  // Return the total size of the message in bytes.
  uint32_t msg_length() const { return msg_buf_.msg_len; }
  // Return the flags of this `MsgBuf'.
  uint16_t flags() const { return msg_buf_.flags; }
  // Returns true if the `MachnetMsgBuf_t' is the first in a message.
  bool is_first() const { return (flags() & MACHNET_MSGBUF_FLAGS_SYN) != 0; }
  // Returns true if the `MachnetMsgBuf_t' is the last in a message.
  bool is_last() const { return (flags() & MACHNET_MSGBUF_FLAGS_FIN) != 0; }
  // Returns true if the `MachnetMsgBuf_t' is the last in a message.
  bool is_sg() const { return (flags() & MACHNET_MSGBUF_FLAGS_SG) != 0; }

  std::string flow_info() const {
    const net::Ipv4::Address src_ip(msg_buf_.flow.src_ip);
    const net::Ipv4::Address dst_ip(msg_buf_.flow.dst_ip);
    const net::Udp::Port src_port(msg_buf_.flow.src_port);
    const net::Udp::Port dst_port(msg_buf_.flow.dst_port);

    return src_ip.ToString() + ":" + std::to_string(src_port.port.value()) +
           " <-> " + dst_ip.ToString() + ":" +
           std::to_string(dst_port.port.value());
  }
  /**
   * @brief Method that returns a pointer to the flow information associated
   * with this `MsgBuf'.
   *
   * @return MachnetFlow_t
   */
  const MachnetFlow_t *flow() const { return &msg_buf_.flow; }

  // Returns true if there is another message buffer in the chain.
  bool has_next() const { return flags() & (MACHNET_MSGBUF_FLAGS_SG); }

  // Returns the next message buffer in the chain.
  bool has_chain() const { return flags() & (MACHNET_MSGBUF_FLAGS_CHAIN); }

  // Returns the next message buffer index in the chain.
  uint32_t next() const { return msg_buf_.next; }

  // Returns the last message buffer index in the chain.
  uint32_t last() const { return msg_buf_.last; }

  // Setters.
  void set_iova(uintptr_t iova) {
    *const_cast<uintptr_t *>(&msg_buf_.iova) = iova;
  }
  void set_length(uint32_t len) { msg_buf_.data_len = len; }
  void set_msg_length(uint32_t len) { msg_buf_.msg_len = len; }
  void set_flags(uint16_t flags) { msg_buf_.flags = flags; }
  void add_flags(uint16_t flags) { msg_buf_.flags |= flags; }
  void set_src_ip(uint32_t ip) { msg_buf_.flow.src_ip = ip; }
  void set_src_port(uint16_t port) { msg_buf_.flow.src_port = port; }
  void set_dst_ip(uint32_t ip) { msg_buf_.flow.dst_ip = ip; }
  void set_dst_port(uint16_t port) { msg_buf_.flow.dst_port = port; }
  void set_next(uint32_t next) {
    msg_buf_.next = next;
    add_flags(MACHNET_MSGBUF_FLAGS_SG);
  }
  void set_next(MsgBuf *next) { set_next(next->index()); }
  void set_last(uint32_t last) { msg_buf_.last = last; }
  void mark_first() { add_flags(MACHNET_MSGBUF_FLAGS_SYN); }
  void mark_last() { add_flags(MACHNET_MSGBUF_FLAGS_FIN); }

  /**
   * @brief Method to link a new message (its first buffer) to the last buffer
   * of a previous one.
   * This is used to chain multiple messages in the context of the transmit
   * queue of a flow.
   * @attention The two messages linked are not associated in any way, and could
   * be delivered separately.
   * TODO(ilias): Ideally find a more elegant solution for this.
   */
  void link(MsgBuf *next) {
    DCHECK(is_last()) << "This is not the last buffer of a message!";
    DCHECK(next->is_first())
        << "The next buffer is not the first of a message!";
    msg_buf_.next = next->index();
    add_flags(MACHNET_MSGBUF_FLAGS_CHAIN);
  }

  /**
   * @brief Prepend len bytes to this buffer and return a pointer to the start
   * address of the prepended data.
   *
   * @param len The number of bytes to prepend.
   * @return A pointer to the start address of the prepended data. If there
   * isn't enough room to prepend, return nullptr, without affecting the
   * underlying message data.
   */
  template <typename T = void *>
  T prepend(uint32_t len) {
    return __machnet_channel_buf_prepend(&msg_buf_, len);
  }

  /**
   * @brief Append len bytes to this buffer and return a pointer to the start
   * address of the appended data.
   *
   * @param len The number of bytes to append.
   * @return A pointer to the start address of the appended data. If there
   * isn't enough room to append, return nullptr, without affecting
   * the underlying message data.
   */
  template <typename T = void *>
  T append(uint32_t len) {
    return __machnet_channel_buf_append(&msg_buf_, len);
  }

 private:
  MachnetMsgBuf_t msg_buf_;
  friend class MsgBufBatch;
};

class MsgBufBatch {
 public:
  static constexpr uint32_t kMaxBurst = 64;
  MsgBufBatch() : cnt_(0), msg_bufs_(), msg_bufs_indices_() {}

  MsgBuf *const *bufs() const { return msg_bufs_; }
  MsgBuf **bufs() { return msg_bufs_; }
  MachnetRingSlot_t *buf_indices() { return msg_bufs_indices_; }

  MsgBuf *operator[](uint16_t index) {
    DCHECK(index < cnt_);
    return msg_bufs_[index];
  }

  uint16_t GetSize() const { return cnt_; }
  uint16_t GetRoom() const { return kMaxBurst - cnt_; }
  bool IsEmpty() { return cnt_ == 0; }
  bool IsFull() { return cnt_ == kMaxBurst; }
  void IncrCount(uint16_t incr) {
    DCHECK(incr <= GetRoom());
    cnt_ += incr;
  }

  // XXX (ilias): Caller is responsible to check there is enough space.
  void Append(MsgBuf *msgbuf, uint32_t msgbuf_index) {
    DCHECK(!IsFull());
    msg_bufs_[cnt_] = msgbuf;
    msg_bufs_indices_[cnt_] = msgbuf_index;
    cnt_++;
  }

  void Append(MsgBuf **msg_bufs, MachnetRingSlot_t *msg_buf_indices,
              uint16_t nbufs) {
    DCHECK(nbufs <= GetRoom());
    juggler::utils::Copy(&msg_bufs_[cnt_], msg_bufs, nbufs * sizeof(MsgBuf *));
    juggler::utils::Copy(&msg_bufs_indices_[cnt_], msg_buf_indices,
                         nbufs * sizeof(*msg_buf_indices));
    cnt_ += nbufs;
  }

  void Append(MsgBufBatch *batch) {
    Append(batch->bufs(), batch->buf_indices(), batch->GetSize());
  }

  void Clear() { cnt_ = 0; }

 private:
  uint16_t cnt_;
  MsgBuf *msg_bufs_[kMaxBurst];
  MachnetRingSlot_t msg_bufs_indices_[kMaxBurst];
};

}  // namespace shm
}  // namespace juggler

#endif  // SRC_INCLUDE_CHANNEL_MSGBUF_H_
