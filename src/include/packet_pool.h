#ifndef SRC_INCLUDE_PACKET_POOL_H_
#define SRC_INCLUDE_PACKET_POOL_H_

#include <packet.h>
#include <rte_common.h>
#include <rte_config.h>
#include <rte_errno.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>

#include <cstdint>

namespace juggler {
namespace dpdk {

class PacketPool {
 public:
  static constexpr uint32_t kRteDefaultMbufsNum_ = 2048 - 1;
  static constexpr uint16_t kRteDefaultMbufDataSz_ = RTE_MBUF_DEFAULT_BUF_SIZE;
  static constexpr char *kRteDefaultMempoolName = nullptr;
  PacketPool() = delete;
  PacketPool(const PacketPool &) = delete;
  PacketPool &operator=(const PacketPool &) = delete;
  PacketPool(uint32_t nmbufs = kRteDefaultMbufsNum_,
             uint16_t mbuf_size = kRteDefaultMbufDataSz_,
             const char *mempool_name = kRteDefaultMempoolName);
  ~PacketPool();

  const char *GetPacketPoolName() { return mpool_->name; }
  uint32_t GetPacketDataRoomSize() const {
    return rte_pktmbuf_data_room_size(mpool_);
  }
  decltype(auto) GetMemPool() { return mpool_; }

  Packet *PacketAlloc() {
    return reinterpret_cast<Packet *>(rte_pktmbuf_alloc(mpool_));
  }

  bool PacketBulkAlloc(Packet **pkts, uint16_t cnt) {
    int ret = rte_pktmbuf_alloc_bulk(
        mpool_, reinterpret_cast<struct rte_mbuf **>(pkts), cnt);
    if (ret == 0) [[likely]] return true;
    return false;
  }

  bool PacketBulkAlloc(PacketBatch *batch, uint16_t cnt) {
    (void)DCHECK_NOTNULL(batch);
    int ret = rte_pktmbuf_alloc_bulk(
        mpool_, reinterpret_cast<struct rte_mbuf **>(batch->pkts()), cnt);
    if (ret != 0) [[unlikely]] return false;

    batch->IncrCount(cnt);
    return true;
  }

  // Returns the total number of packets in the pool.
  uint32_t Capacity() { return mpool_->populated_size; }
  uint32_t AvailPacketsCount() { return rte_mempool_avail_count(mpool_); }

 private:
  const bool is_dpdk_primary_process_;
  static uint16_t next_id_;
  rte_mempool
      *mpool_;  // The underlying rte mbufpool that backs this packet pool.
  uint16_t id_;
};

}  // namespace dpdk
}  // namespace juggler

#endif  // SRC_INCLUDE_PACKET_POOL_H_
