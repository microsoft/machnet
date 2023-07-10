#ifndef SRC_INCLUDE_PACKET_H_
#define SRC_INCLUDE_PACKET_H_

#include <common.h>
#include <ether.h>
#include <glog/logging.h>
#include <ipv4.h>
#include <rte_mbuf.h>
#include <utils.h>
#include <x86intrin.h>

#include <cstdint>

namespace juggler {
namespace dpdk {

class alignas(juggler::hardware_constructive_interference_size) Packet {
 public:
  // Packet can only be allocated from a PacketPool.
  Packet() = delete;

  // Free the packet: return the corresponding rte_mbuf back in the mempool.
  // pkt may be nullptr.
  static void Free(Packet *pkt) { rte_pktmbuf_free(&pkt->mbuf_); }

  static void Reset(Packet *pkt) {
    auto &mbuf_ = pkt->mbuf_;
    struct rte_mempool *mp = mbuf_.pool;
    uint32_t mbuf_size, buf_len;
    uint16_t priv_size;

    priv_size = rte_pktmbuf_priv_size(mp);
    mbuf_size = static_cast<uint32_t>(sizeof(struct rte_mbuf) + priv_size);
    buf_len = rte_pktmbuf_data_room_size(mp);

    mbuf_.priv_size = priv_size;
    mbuf_.buf_addr = reinterpret_cast<uint8_t *>(&mbuf_) + mbuf_size;
    mbuf_.buf_iova = rte_mempool_virt2iova(&mbuf_) + mbuf_size;
    mbuf_.buf_len = static_cast<uint16_t>(buf_len);
    rte_pktmbuf_reset_headroom(&mbuf_);
    mbuf_.data_len = 0;
    mbuf_.ol_flags = 0;
    mbuf_.shinfo = nullptr;
    rte_mbuf_refcnt_set(&mbuf_, 1);
    mbuf_.next = nullptr;
    mbuf_.nb_segs = 1;
  }

  template <typename T = void *>
  const T head_data(uint16_t offset = 0) const {
    return reinterpret_cast<T>(rte_pktmbuf_mtod(&mbuf_, uint8_t *) + offset);
  }

  template <typename T = void *>
  T head_data(uint16_t offset = 0) {
    return const_cast<T>(
        static_cast<const Packet &>(*this).head_data<T>(offset));
  }

  uint16_t length() const { return rte_pktmbuf_pkt_len(&mbuf_); }
  uint32_t rss_hash() const { return mbuf_.hash.rss; }

  // Setters.
  void set_l2_len(uint16_t length) { mbuf_.l2_len = length; }
  void set_l3_len(uint16_t length) { mbuf_.l3_len = length; }
  void offload_ipv4_csum() {
    mbuf_.ol_flags |= (RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM);
  }
  void offload_udpv4_csum() {
    offload_ipv4_csum();
    mbuf_.ol_flags |= (RTE_MBUF_F_TX_UDP_CKSUM);
  }

  /**
   * @brief Attach external buffer to this packet mbuf.
   * @param buf_va External buffer virtual address (VA).
   * @param buf_iova External buffer IO address (IOVA).
   * @param buf_len Total length of the external buffer.
   * @param buf_data_ofs Offset of the data in the external buffer.
   * @param buf_data_len Length of the data in the external buffer.
   * @param shinfo DPDK-related shared info of the external buffer.
   */
  void attach_extbuf(void *buf_va, uint64_t buf_iova, uint16_t buf_len,
                     uint32_t buf_data_ofs, uint32_t buf_data_len,
                     rte_mbuf_ext_shared_info *shinfo) {
    rte_pktmbuf_attach_extbuf(&mbuf_, buf_va, buf_iova, buf_len, shinfo);
    mbuf_.data_off = buf_data_ofs;
    mbuf_.data_len = buf_data_len;
    mbuf_.pkt_len = buf_data_len;
    rte_mbuf_refcnt_set(&mbuf_, 1);
  }

  /**
   * @brief Append len bytes to this packet and return a pointer to the start
   * address of the appended data.
   *
   * If there isn't enough room to append, return nullptr, without affecting
   * the underlying packet.
   */
  template <typename T = void *>
  T append(uint16_t len) {
    return reinterpret_cast<T>(rte_pktmbuf_append(&mbuf_, len));
  }

  /**
   * @brief Prepend len bytes to this packet and return a pointer to the start
   * address of the prepended data.
   *
   * If there isn't enough room to prepend, return nullptr, without affecting
   * the underlying packet.
   */
  template <typename T = void *>
  T prepend(uint16_t len) {
    return reinterpret_cast<T>(rte_pktmbuf_prepend(&mbuf_, len));
  }

  std::string L2L3HeaderString() const {
    auto *eh = head_data<net::Ethernet *>();
    auto *ipv4h = reinterpret_cast<net::Ipv4 *>(eh + 1);
    return eh->ToString() + " " + ipv4h->ToString();
  }

 private:
  struct rte_mbuf mbuf_;

  friend class PacketPool;
  friend class PacketBatch;
};

class PacketBatch {
 public:
  static const uint16_t kMaxBurst = 32;
  PacketBatch() : cnt_(0), pkts_() {}

  Packet *const *pkts() const { return pkts_; }
  Packet **pkts() { return pkts_; }

  Packet *operator[](uint16_t index) {
    DCHECK(index < cnt_);
    return pkts_[index];
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
  void Append(Packet *pkt) {
    DCHECK(!IsFull());
    pkts_[cnt_++] = pkt;
  }
  void Append(Packet **pkts, uint16_t npkts) {
    DCHECK(npkts <= GetRoom());
    juggler::utils::Copy(&pkts_[cnt_], pkts, npkts * sizeof(Packet *));
    cnt_ += npkts;
  }
  void Append(PacketBatch *batch) { Append(batch->pkts(), batch->GetSize()); }

  void Clear() { cnt_ = 0; }

  /// Return the packets in this batch to their mempools
  void Release() {
    if (cnt_ == 0) [[unlikely]] return;
    rte_pktmbuf_free_bulk(reinterpret_cast<rte_mbuf **>(pkts_), cnt_);
    Clear();
  }

 private:
  uint16_t cnt_;
  Packet *pkts_[kMaxBurst];
};
}  // namespace dpdk
}  // namespace juggler

#endif  // SRC_INCLUDE_PACKET_H_
