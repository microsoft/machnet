/**
 * @file shm.cc
 *
 * Implementation of `Channel' and `ChannelManager' methods.
 */
#include <channel.h>
#include <flow.h>
#include <glog/logging.h>

namespace juggler {
namespace shm {

ShmChannel::ShmChannel(const std::string channel_name,
                       const MachnetChannelCtx_t *channel_ctx,
                       const size_t channel_mem_size, const bool is_posix_shm,
                       int channel_fd)
    : name_(channel_name),
      ctx_(CHECK_NOTNULL(channel_ctx)),
      mem_size_(channel_mem_size),
      is_posix_shm_(is_posix_shm),
      channel_fd_(channel_fd),
      cached_buf_indices(),
      cached_bufs(),
      cached_buf_count(0) {}

ShmChannel::~ShmChannel() {
  __machnet_channel_destroy(
      const_cast<void *>(reinterpret_cast<const void *>(ctx_)), mem_size_,
      &channel_fd_, is_posix_shm_, name_.c_str());
}

Channel::Channel(const std::string &channel_name,
                 const MachnetChannelCtx_t *channel_ctx,
                 const size_t channel_mem_size, const bool is_posix_shm,
                 int channel_fd)
    : ShmChannel(channel_name, channel_ctx, channel_mem_size, is_posix_shm,
                 channel_fd),
      listeners_(),
      active_flows_() {}

Channel::~Channel() { UnregisterDMAMem(); }

bool Channel::RegisterMemForDMA(rte_device *dev) {
  const auto *bufp_mem_start = GetBufPoolAddr();
  const auto *bufp_mem_end = GetBufPoolAddr() + GetBufPoolSize();
  const size_t page_size = IsPosixShm() ? kPageSize : kHugePage2MSize;

  if (attached_dev_ != nullptr) {
    LOG(ERROR) << "Memory is already registered with DPDK";
    return false;
  }

  // Check that the memory is page-aligned.
  if (reinterpret_cast<uintptr_t>(bufp_mem_start) & (page_size - 1)) {
    LOG(ERROR) << "Channel memory is not page-aligned (page size: " << page_size
               << ")";
    return false;
  }

  // There is padding at the end of the memory region to make sure that the end
  // is page-aligned.
  LOG_IF(FATAL, bufp_mem_end > __machnet_channel_end(ctx()))
      << "Out of bounds!";

  // Calculate the number of pages in the memory region, and the IOVA addresses
  // of each one.
  const auto pages_nr = (GetBufPoolSize() + page_size - 1) / page_size;
  buffer_pages_va_.resize(pages_nr);
  buffer_pages_iova_.resize(pages_nr);
  LOG(INFO) << "Registering " << pages_nr << " pages of size " << page_size
            << " bytes";
  for (auto i = 0u; i < pages_nr; ++i) {
    const auto *page_addr = bufp_mem_start + i * page_size;
    buffer_pages_va_[i] =
        const_cast<void *>(static_cast<const void *>(page_addr));
    buffer_pages_iova_[i] = rte_mem_virt2phy(page_addr);
    LOG(INFO) << "Page " << i << " at " << buffer_pages_va_[i] << " has IOVA "
              << buffer_pages_iova_[i];
    if (buffer_pages_iova_[i] == RTE_BAD_IOVA) {
      LOG(ERROR) << "Failed to get IOVA for page " << i;
      return false;
    }
  }

  // Update the IOVA addresses of the buffers in the buffer pool.
  for (auto i = 0u; i < GetTotalBufCount(); ++i) {
    auto *msg_buf = GetMsgBuf(i);
    const auto *buf_va = msg_buf->base();
    msg_buf->set_iova(rte_mem_virt2phy(buf_va));
    if (msg_buf->iova() == RTE_BAD_IOVA) {
      LOG(ERROR) << utils::Format("Failed to get IOVA for buffer@%p)", buf_va);
      return false;
    }
  }

  // Register external memory with DPDK.
  const auto ret = rte_extmem_register(
      const_cast<void *>(GetBufPoolAddr<void *>()), pages_nr * page_size,
      buffer_pages_iova_.data(), buffer_pages_iova_.size(), page_size);
  if (ret != 0) {
    LOG(ERROR) << "Failed to register external memory with DPDK ("
               << rte_strerror(rte_errno) << ")";
    return false;
  }

  // If memory was registered successfully, we now need to dma map it.
  for (auto i = 0u; i < pages_nr; ++i) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    const auto ret = rte_dev_dma_map(dev, buffer_pages_va_[i],
                                     buffer_pages_iova_[i], page_size);
#pragma GCC diagnostic pop
    if (ret != 0) {
      LOG(ERROR) << utils::Format(
          "Failed to DMA map page %u (VA: %p, IOVA: %p, size: %zu) (%s)", i,
          buffer_pages_va_[i], static_cast<uintptr_t>(buffer_pages_iova_[i]),
          page_size, rte_strerror(rte_errno));
      return false;
    }
    LOG(INFO) << utils::Format(
        "[+] DMA mapping: VA [%p, %p) - IOVA [%p, %p)", buffer_pages_va_[i],
        static_cast<uchar_t *>(buffer_pages_va_[i]) + page_size,
        static_cast<uintptr_t>(buffer_pages_iova_[i]),
        static_cast<uintptr_t>(buffer_pages_iova_[i] + page_size));
  }

  attached_dev_ = dev;

  return true;
}

void Channel::UnregisterDMAMem() {
  if (attached_dev_ == nullptr) {
    LOG(ERROR) << "Memory is not registered with DPDK";
    return;
  }

  const size_t page_size = IsPosixShm() ? kPageSize : kHugePage2MSize;
  for (auto i = 0u; i < buffer_pages_va_.size(); ++i) {
    LOG(INFO) << utils::Format(
        "[-] DMA unmapping: VA [%p, %p) - IOVA [%p, %p)", buffer_pages_va_[i],
        static_cast<uchar_t *>(buffer_pages_va_[i]) + page_size,
        static_cast<uintptr_t>(buffer_pages_iova_[i]),
        static_cast<uintptr_t>(buffer_pages_iova_[i]) + page_size);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    const auto ret = rte_dev_dma_unmap(attached_dev_, buffer_pages_va_[i],
                                       buffer_pages_iova_[i], page_size);
#pragma GCC diagnostic pop
    if (ret != 0) {
      LOG(ERROR) << utils::Format(
          "Failed to DMA unmap page %u (VA: %p, IOVA: %p, size: %zu) (%s)", i,
          buffer_pages_va_[i], static_cast<uintptr_t>(buffer_pages_iova_[i]),
          page_size, rte_strerror(rte_errno));
    }
  }

  const auto pages_nr = buffer_pages_va_.size();
  const auto ret = rte_extmem_unregister(
      const_cast<void *>(GetBufPoolAddr<void *>()), pages_nr * page_size);
  if (ret != 0) {
    LOG(ERROR) << "Failed to unregister external memory with DPDK ("
               << rte_strerror(rte_errno) << ")";
  }

  attached_dev_ = nullptr;
}

void Channel::RemoveFlow(
    const std::list<std::unique_ptr<Flow>>::const_iterator &flow_it) {
  active_flows_.erase(flow_it);
}

}  // namespace shm
}  // namespace juggler
