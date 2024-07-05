/**
 * @file channel.h
 *
 * Machnet Channel abstraction and management classes.
 */
#ifndef SRC_INCLUDE_CHANNEL_H_
#define SRC_INCLUDE_CHANNEL_H_

#include <channel_msgbuf.h>
#include <common.h>
#include <flow_key.h>
#include <glog/logging.h>
#include <machnet_common.h>
#include <machnet_private.h>
#include <rte_dev.h>
#include <rte_eal.h>
#include <rte_mbuf_core.h>

#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace juggler {
class MachnetEngine;  // forward declaration
}

namespace juggler {
namespace net {
namespace flow {
class Flow;  // forward declaration
}  // namespace flow
}  // namespace net
}  // namespace juggler

namespace juggler {
namespace shm {

/**
 * @brief Class `ShmChannel' abstracts Machnet shared memory channels.
 * It provides useful methods to allocate, enqueue and dequeue messages to
 * channels.
 *
 * This class is non-copyable.
 */
class ShmChannel {
 public:
  using Flow = juggler::net::flow::Flow;
  using Listener = juggler::net::flow::Listener;
  ShmChannel() = delete;
  ShmChannel(const ShmChannel &) = delete;
  /**
   * @brief `ShmChannel' Constructor.
   * @param channel_name  The name of the channel.
   * @param channel_ctx   The Machnet channel context.
   * @param channel_mem_size The size of the underlying memory region. This can
   * be different that the size of memory used by the channel (e.g., memory
   * region might be aligned to huge page size)
   * @param is_posix_shm  Whether the channel is backed by a POSIX shared memory
   * or anonymous huge pages.
   * @param channel_fd    The file descriptor of the channel.
   */
  ShmChannel(const std::string channel_name,
             const MachnetChannelCtx_t *channel_ctx,
             const size_t channel_mem_size, const bool is_posix_shm,
             int channel_fd);
  ~ShmChannel();
  ShmChannel &operator=(const ShmChannel &) = delete;

  // Return a pointer to the channel's context.
  MachnetChannelCtx_t *ctx() const {
    return const_cast<MachnetChannelCtx_t *>(ctx_);
  }

  // Get the channel's file descriptor.
  int GetFd() const { return channel_fd_; }

  // Get the name of this channel.
  std::string GetName() const { return name_; }

  // Get the address of the channel's buffer pool.
  template <typename T = uchar_t *>
  const T GetBufPoolAddr() const {
    return reinterpret_cast<T>(__machnet_channel_buf_pool(ctx()));
  }

  // Get total buffer pool size in bytes.
  size_t GetBufPoolSize() const {
    return __machnet_channel_buf_pool_size(ctx());
  }

  // Is this channel backed by a POSIX shared memory?
  bool IsPosixShm() const { return is_posix_shm_; }

  // Size of the channel in bytes.
  uint64_t GetSize() const { return ctx_->size; }

  // Total size of each channel's buffer in bytes.
  uint32_t GetTotalBufSize() const { return ctx_->data_ctx.buf_size; }

  // Machnet channel `MsgBuf' have reserved space for headers.
  // This method returns the space in the `MsgBuf' that can be used to store the
  // payload. There is still headroom for possible packet headers.
  uint32_t GetUsableBufSize() const { return ctx_->data_ctx.buf_mss; }

  // Total amount of buffers in the channel.
  uint32_t GetTotalBufCount() const {
    return __machnet_channel_buf_ring(ctx_)->capacity;
  }

  // Get the number of buffers that are currently available (i.e., not in use).
  uint32_t GetFreeBufCount() const {
    return cached_buf_count + __machnet_channel_buffers_avail(ctx_);
  }

  /**
   * @brief Returns a pointer to a `MsgBuf' object based on the index of the
   * buffer.
   *
   * @param index       The index of the buffer.
   * @return MsgBuf*     A pointer to the buffer.
   */
  MsgBuf *GetMsgBuf(MachnetRingSlot_t index) {
    return reinterpret_cast<MsgBuf *>(__machnet_channel_buf(ctx_, index));
  }

  /**
   * @brief Given the pointer to a `MsgBuf' object, returns the index of the
   * buffer on the channel.
   *
   * @param MsgBuf*     A pointer to the buffer.
   * @return index      The index of the buffer.
   */
  uint32_t GetBufIndex(MsgBuf *msg_buf) {
    return __machnet_channel_buf_index(
        ctx_, reinterpret_cast<MachnetMsgBuf_t *>(msg_buf));
  }

  /**
   * @brief Dequeues pending control work queue entries from the channel.
   *
   * @param ctrl_entries  A pointer to the array of `MachnetCtrlQueueEntry_t'
   * @param nb_entries    The max number of entries in the array above.
   * @return              The number of entries dequeued.
   */
  uint32_t DequeueCtrlRequests(MachnetCtrlQueueEntry_t *ctrl_entries,
                               uint32_t nb_entries) {
    return __machnet_channel_ctrl_sq_dequeue(ctx_, nb_entries, ctrl_entries);
  }

  /**
   * @brief Enqueue a batch of WQE completions (CQE) to the channel (destined to
   * the application).
   *
   * @param ctrl_entries  A pointer to the array of `MachnetCtrlQueueEntry_t'.
   * @param nb_entries    The number of entries in the array above.
   * @return              The number of entries enqueued.
   */
  uint32_t EnqueueCtrlCompletions(MachnetCtrlQueueEntry_t *ctrl_entries,
                                  uint32_t nb_entries) {
    return __machnet_channel_ctrl_cq_enqueue(ctx_, nb_entries, ctrl_entries);
  }

  /**
   * @brief Enqueues a batch of messages to the channel (destined to the
   * application).
   *
   * @param msgbuf_indices   A pointer to the array of `MsgBuf' indices.
   * @param nb_msgs          The number of entries in the array above.
   * @return                 The number of messages enqueued.
   */
  uint32_t EnqueueMessages(MachnetRingSlot_t *msgbuf_indices,
                           uint32_t nb_msgs) {
    return __machnet_channel_machnet_ring_enqueue(ctx_, nb_msgs,
                                                  msgbuf_indices);
  }

  /**
   * @brief Enqueues a batch of messages to the channel (destined to the
   * application).
   *
   * @param msgs        A pointer to the array of pointers to messages to be
   *                    enqueued.
   * @param nb_msgs     The number of messages enqueued (Limited to
   *                    `MsgBufBatch::kMaxBurst')
   * @return           The number of messages enqueued.
   */
  uint32_t EnqueueMessages(MsgBuf *const *msgs, uint32_t nb_msgs) {
    MachnetRingSlot_t slots[MsgBufBatch::kMaxBurst];
    auto nmsgs = std::min(nb_msgs, MsgBufBatch::kMaxBurst);

    for (uint32_t i = 0; i < nmsgs; i++) {
      slots[i] = __machnet_channel_buf_index(
          ctx_, reinterpret_cast<const MachnetMsgBuf_t *>(msgs[i]));
    }

    return EnqueueMessages(slots, nmsgs);
  }

  /**
   * @brief Enqueues a batch of messages to the channel (destined to the
   * application).
   *
   * @param batch       The batch of messages to enqueue.
   * @return uint32_t   The number of messages enqueued.
   */
  uint32_t EnqueueMessages(MsgBufBatch *batch) {
    return EnqueueMessages(batch->buf_indices(), batch->GetSize());
  }

  /**
   * @brief Dequeues a number of messages from the channel (destined to the
   * Machnet stack).
   *
   * @param msg_indices        A pointer to the array of `MachnetRingSlot_t'
   *                           objects (indices of buffers).
   * @param msgs               A pointer to the array of pointers to `MsgBuf'
   *                           objects.
   * @param nb_msgs            The number of messages to dequeue.
   */
  uint32_t DequeueMessages(MachnetRingSlot_t *msg_indices, MsgBuf **msgs,
                           uint32_t nb_msgs) {
    uint32_t ret =
        __machnet_channel_app_ring_dequeue(ctx_, nb_msgs, msg_indices);
    for (uint32_t i = 0; i < ret; i++) {
      msgs[i] = reinterpret_cast<MsgBuf *>(
          __machnet_channel_buf(ctx_, msg_indices[i]));
    }

    return ret;
  }

  /**
   * @brief Dequeues a batch of messages from the channel (destined to the
   * Machnet stack).
   *
   * @param batch       A pointer to an empty `MsgBufBatch' object to hold the
   *                    dequeued messages.
   * @return uint32_t   The number of messages dequeued.
   */
  uint32_t DequeueMessages(MsgBufBatch *batch) {
    (void)DCHECK_NOTNULL(batch);
    auto ret =
        DequeueMessages(&batch->buf_indices()[batch->GetSize()],
                        &batch->bufs()[batch->GetSize()], batch->GetRoom());
    batch->IncrCount(ret);
    return ret;
  }

  /**
   * @brief Allocates a single message buffer from the channel.
   *
   * @return - pointer to the buffer on success, nullptr otherwise.
   */
  MsgBuf *MsgBufAlloc() {
    if (cached_buf_count == 0) {
      uint32_t ret = __machnet_channel_buf_alloc_bulk(
          ctx_, NUM_CACHED_BUFS, cached_buf_indices.data(), cached_bufs.data());
      if (ret != NUM_CACHED_BUFS) return nullptr;
      cached_buf_count += NUM_CACHED_BUFS;
    }
    MachnetMsgBuf_t *buf = cached_bufs[--cached_buf_count];
    __machnet_channel_buf_init(buf);
    return reinterpret_cast<MsgBuf *>(buf);
  }

  /**
   * @brief Releases a single message buffer back to the channel.
   *
   * @param buf     The message buffer to release.
   * @return        true on success, false otherwise.
   */
  bool MsgBufFree(MsgBuf *buf) {
    (void)DCHECK_NOTNULL(buf);
    MachnetRingSlot_t index[1] = {__machnet_channel_buf_index(
        ctx_, reinterpret_cast<const MachnetMsgBuf_t *>(buf))};
    MachnetMsgBuf_t *msg_buf = __machnet_channel_buf(ctx_, index[0]);

    if (cached_buf_count < NUM_CACHED_BUFS) {
      cached_buf_indices[cached_buf_count] = index[0];
      cached_bufs[cached_buf_count] = msg_buf;
      cached_buf_count++;
      return true;
    }

    int retries = 5;
    unsigned int ret;
    do {
      ret = __machnet_channel_buf_free_bulk(ctx_, 1, index);
    } while (ret == 0 && retries-- > 0);

    return ret;
  }

  /**
   * @brief Allocates a batch of message buffers from the channel.
   *
   * @param batch       A pointer to an empty `MsgBufBatch' object to hold the
   *                    allocated buffers.
   * @param cnt         The number of buffers to allocate.
   * @return 'true' on success, 'false' otherwise.
   */
  bool MsgBufBulkAlloc(MsgBufBatch *batch,
                       uint32_t cnt = MsgBufBatch::kMaxBurst) {
    (void)DCHECK_NOTNULL(batch);
    uint32_t ret = __machnet_channel_buf_alloc_bulk(
        ctx_, std::min(cnt, static_cast<uint32_t>(batch->GetRoom())),
        batch->buf_indices(),
        reinterpret_cast<MachnetMsgBuf_t **>(batch->bufs()));
    batch->IncrCount(ret);
    if (ret == 0) [[unlikely]]
      return false;
    return true;
  }

  /**
   * @brief Releases a batch of `MsgBuf' objects to the channel.
   *
   * @param batch       A pointer to a `MsgBufBatch' object.
   * @return            True on success, false otherwise.
   */
  bool MsgBufBulkFree(MsgBufBatch *batch) {
    (void)DCHECK_NOTNULL(batch);

    if (batch->GetSize() == 0) [[unlikely]]
      return true;  // NOLINT

    auto ret = MsgBufBulkFree(batch->buf_indices(), batch->GetSize());

    if (ret == 0) [[unlikely]]
      return false;  // NOLINT
    batch->Clear();
    return true;
  }

  bool MsgBufBulkFree(MachnetRingSlot_t *indices, uint32_t cnt) {
    int retries = 5;
    uint32_t freed;
    const uint32_t cache_free_slots = NUM_CACHED_BUFS - cached_buf_count;
    const uint32_t to_cache =
        (cnt <= cache_free_slots) ? cnt : cache_free_slots;

    for (freed = 0; freed < to_cache; freed++) {
      cached_buf_indices[cached_buf_count] = indices[freed];
      MachnetMsgBuf_t *msg_buf = __machnet_channel_buf(ctx_, indices[freed]);
      cached_bufs[cached_buf_count] = msg_buf;
      cached_buf_count++;
    }
    if (cnt > cache_free_slots) {
      do {
        freed +=
            __machnet_channel_buf_free_bulk(ctx_, cnt - freed, indices + freed);
      } while (freed == 0 && retries-- > 0);
    }
    if (freed == 0) [[unlikely]]
      return false;  // NOLINT

    return true;
  }

  uint32_t GetAllCachedBufferIndices(std::vector<MachnetRingSlot_t> *indices) {
    indices->insert(indices->end(), cached_buf_indices.begin(),
                    cached_buf_indices.begin() + cached_buf_count);
    uint32_t ret = cached_buf_count;
    cached_buf_count = 0;
    return ret;
  }

 private:
  const std::string name_;
  const MachnetChannelCtx_t *ctx_;
  const size_t mem_size_;
  const bool is_posix_shm_;
  int channel_fd_;
  std::array<MachnetRingSlot_t, NUM_CACHED_BUFS> cached_buf_indices;
  std::array<MachnetMsgBuf_t *, NUM_CACHED_BUFS> cached_bufs;
  uint32_t cached_buf_count;
};

/**
 * @brief Class `Channel' abstracts Machnet shared memory channels and provides
 * some extra functionality (compared the base `ShmChannel') to associate
 * channels with flows and listeners which are used by the Machnet engine.
 *
 * This class is non-copyable.
 */
class Channel : public ShmChannel {
 public:
  Channel() = delete;
  Channel(const Channel &) = delete;
  /**
   * @brief `ShmChannel' Constructor.
   * @param channel_name  The name of the channel.
   * @param channel_ctx   The Machnet channel context.
   * @param channel_mem_size The size of the underlying memory region. This can
   * be different that the size of memory used by the channel (e.g., memory
   * region might be aligned to huge page size)
   * @param is_posix_shm  Whether the channel is backed by a POSIX shared memory
   * or anonymous huge pages.
   * @param channel_fd    The file descriptor of the channel.
   */
  Channel(const std::string &name, const MachnetChannelCtx_t *ctx,
          const size_t channel_mem_size, const bool is_posix_shm,
          int channel_fd);
  ~Channel();
  Channel &operator=(const Channel &) = delete;

  /**
   * @brief Get a pointer to mbuf shinfo structure for this channel's buffers.
   * @return A pointer to the mbuf shinfo structure.
   */
  rte_mbuf_ext_shared_info *GetMbufExtShinfo() { return &sh_info_; }

  /**
   * @brief Register `Channel' memory as DPDK external memory.
   * @return True on success, false otherwise.
   */
  bool RegisterMemForDMA(rte_device *dev);

  /**
   * @brief Unregister `Channel' memory for DMA access.
   */
  void UnregisterDMAMem();

 protected:
  /**
   * @brief Gets the list of active flows.
   * @return A reference to the list of active flows.
   */
  std::list<std::unique_ptr<Flow>> &GetActiveFlows() { return active_flows_; }

  /**
   * @brief Gets the list of listeners associated with the channel.
   * @return A reference to the list of listeners.
   */
  std::unordered_set<Listener> &GetListeners() { return listeners_; }

  /**
   * @brief Creates a new flow associated with `this' channel object.
   * @param params The parameters pack to be forwarded to the constructor of the
   *               Flow.
   * @return A const iterator to the newly created flow.
   */
  const std::list<std::unique_ptr<Flow>>::const_iterator CreateFlow(
      auto &&...params) {
    active_flows_.emplace_back(std::make_unique<Flow>(
        std::forward<decltype(params)>(params)..., this));
    return std::prev(active_flows_.end());
  }

  void RemoveFlow(
      const std::list<std::unique_ptr<Flow>>::const_iterator &flow_it);

  /**
   * @brief Adds a listener to the channel (i.e., an IP address and port pair).
   * @param params The parameters pack to be forwarded to the constructor of the
   *               Listener.
   */
  void AddListener(auto &&...params) {
    Listener listener{std::forward<decltype(params)>(params)...};
    CHECK(listeners_.find(listener) == listeners_.end())
        << "Listener already exists for channel " << GetName();
    listeners_.insert(listener);
  }

 private:
  static void free_ext_buf_cb(void *arg, void *opaque) {
    // Empty callback.

    // DPDK requires a callback to be registered with the mbuf shinfo in the
    // case of external buffers. The purpose of this callback is to do any post
    // mbuf release cleanup.

    // There is a caveat with this mechanism: The callback is only called by
    // DPDK if the `FAST_FREE' offload is not set and the reference count has
    // reached zero. With `FAST_FREE' offload enabled, the mbuf is simply put
    // back to the relevant pool with only minimal initialization. We do not
    // need this callback to re-initialize mbufs; we explicitly initialize them
    // in the stack and no further book-keeping is required for channel buffers.
  }

  rte_mbuf_ext_shared_info sh_info_{
      .free_cb = free_ext_buf_cb, .fcb_opaque = nullptr, .refcnt = 0};

  // List of listeners associated with this channel.
  std::unordered_set<Listener> listeners_;
  // List of active flows associated with this channel.
  std::list<std::unique_ptr<Flow>> active_flows_;

  // DPDK external memory region.
  rte_device *attached_dev_{nullptr};
  std::vector<void *> buffer_pages_va_{};
  std::vector<uint64_t> buffer_pages_iova_{};

  friend class juggler::MachnetEngine;
  template <class T>
  class ChannelManager;
};

/**
 * @brief Class `ChannelManager' is a class that can be used to manage channels.
 * A `ChannelManager' provides method to create, destroy and access the
 * underlying Machnet Channels it holds.
 */
template <class T = Channel,
          class =
              typename std::enable_if<std::is_same<T, Channel>::value ||
                                      std::is_same<T, ShmChannel>::value>::type>
class ChannelManager {
 public:
  static constexpr size_t kMaxChannelNr = 32;
  static constexpr size_t kDefaultRingSize = 256;
  static constexpr size_t kDefaultBufferCount = 4096;
  ChannelManager() {}
  ChannelManager(const ChannelManager &) = delete;
  ChannelManager &operator=(const ChannelManager &) = delete;

  /**
   * Create a new Machnet dataplane channel.
   *
   * @param name               Name of the channel (POSIX shared memory
   *                           segment)
   * @param machnet_ring_slot_nr The number of Machnet->App messaging ring slots
   *                           (must be power of 2).
   * @param app_ring_slot_nr   The number of App->Machnet messaging ring slots
   *                           (must be power of 2).
   * @param buf_ring_slot_nr   The number of buffers + 1 in the pool (must be
   *                           power of 2).
   * @param buffer_size        The size of each buffer (power of 2).
   * @return
   *   - `true` if the channel was successfully created.
   *   - `false` otherwise.
   */
  bool AddChannel(const char *name, size_t machnet_ring_slot_nr,
                  size_t app_ring_slot_nr, size_t buf_ring_slot_nr,
                  size_t buffer_size) {
    const std::lock_guard<std::mutex> lock(mtx_);
    if (channels_.size() >= kMaxChannelNr) {
      LOG(WARNING) << "Too many channels.";
      return false;
    }

    if (channels_.find(name) != channels_.end()) {
      LOG(WARNING) << "Channel " << name << " already exists.";
      return false;
    }

    int channel_fd;
    size_t shm_segment_size;
    int is_posix_shm;
    auto *ctx = __machnet_channel_create(
        name, machnet_ring_slot_nr, app_ring_slot_nr, buf_ring_slot_nr,
        buffer_size, &shm_segment_size, &is_posix_shm, &channel_fd);
    if (ctx == nullptr) {
      LOG(WARNING) << "Failed to create channel " << name
                   << " with requested size " << shm_segment_size << ".";
      return false;
    }

    channels_.insert(
        std::make_pair(name, std::make_shared<T>(name, ctx, shm_segment_size,
                                                 is_posix_shm, channel_fd)));
    return true;
  }

  /**
   * @brief Request the manager to remove a channel. The action will take
   * effect on the next call to `Update()`.
   *
   * @param name  Name of the channel (POSIX shared memory segment).
   */
  void DestroyChannel(const char *name) {
    const std::lock_guard<std::mutex> lock(mtx_);

    if (channels_.find(name) != channels_.end()) channels_.erase(name);
  }

  /**
   * @brief Get a smart pointer to a channel held by this manager.
   *
   * @param name The name of the Channel to get.
   * @return std::shared_ptr<T>  A smart pointer to the channel (nullptr
   *         on failure).
   */
  std::shared_ptr<T> GetChannel(const char *name) {
    const std::lock_guard<std::mutex> lock(mtx_);
    if (channels_.find(name) == channels_.end()) return nullptr;

    return channels_[name];
  }

  /**
   * @brief Get all the channels held by this manager.
   * @return std::vector<std::shared_ptr<T>>  A vector of smart pointers
   *         to the channels.
   */
  std::vector<std::shared_ptr<T>> GetAllChannels() {
    const std::lock_guard<std::mutex> lock(mtx_);
    std::vector<std::shared_ptr<T>> channels;
    for (const auto &c : channels_) channels.push_back(c.second);

    return channels;
  }

  // Return the number of channels held by this manager.
  size_t GetChannelCount() {
    const std::lock_guard<std::mutex> lock(mtx_);
    return channels_.size();
  }

 private:
  std::mutex mtx_;
  std::unordered_map<std::string, std::shared_ptr<T>> channels_;
};

}  // namespace shm
}  // namespace juggler

#endif  // SRC_INCLUDE_CHANNEL_H_
