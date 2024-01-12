#ifndef SRC_INCLUDE_PMD_H_
#define SRC_INCLUDE_PMD_H_

#include <glog/logging.h>
#include <rte_bus_pci.h>
#include <rte_ethdev.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "dpdk.h"
#include "ether.h"
#include "packet.h"
#include "packet_pool.h"

namespace juggler {
namespace dpdk {

// Forward declarations.
class PmdPort;

/**
 * @brief Base class for RX and TX rings.
 *
 * Represents an abstraction for RX and TX rings used in DPDK.
 */
class PmdRing {
 public:
  static const inline uint16_t kDefaultFrameSize = 1500;
  static const inline uint16_t kJumboFrameSize =
      9000 - RTE_ETHER_HDR_LEN - RTE_ETHER_CRC_LEN;
  static const inline uint16_t kDefaultRingDescNr = 512;

  /**
   * @brief Default and copy constructors are deleted to prevent instantiation
   * and copying.
   */
  PmdRing() = delete;
  PmdRing(PmdRing const &) = delete;
  PmdRing &operator=(PmdRing const &) = delete;

  virtual ~PmdRing() = default;

  const PmdPort *GetPmdPort() const { return pmd_port_; }
  PacketPool *GetPacketPool() const { return ppool_.get(); }
  uint16_t GetDescNum() const { return ndesc_; }
  uint8_t GetPortId() const { return port_id_; }
  uint16_t GetRingId() const { return ring_id_; }

 protected:
  // Only TX rings can be initialized without a packetpool attached.
  PmdRing(const PmdPort *port, uint8_t port_id, uint16_t ring_id,
          uint16_t ndesc)
      : pmd_port_(port),
        port_id_(port_id),
        ring_id_(ring_id),
        ndesc_(ndesc),
        ppool_(nullptr) {}
  PmdRing(const PmdPort *port, uint8_t port_id, uint16_t ring_id,
          uint16_t ndesc, uint32_t nmbufs, uint32_t mbuf_sz)
      : pmd_port_(port),
        port_id_(port_id),
        ring_id_(ring_id),
        ndesc_(ndesc),
        ppool_(std::unique_ptr<PacketPool>(new PacketPool(nmbufs, mbuf_sz))) {}

  rte_mempool *GetPacketMemPool() const { return ppool_.get()->GetMemPool(); }

 private:
  const PmdPort *pmd_port_;
  const uint8_t port_id_;
  const uint16_t ring_id_;
  const uint16_t ndesc_;
  const std::unique_ptr<PacketPool> ppool_;
};

/*
 * @brief Represents a TX ring in DPDK.
 *
 * Provides functionalities specific to TX rings (e.g., sending packets).
 */
class TxRing : public PmdRing {
 public:
  TxRing(const PmdPort *pmd_port, uint8_t port_id, uint16_t ring_id,
         uint16_t ndesc)
      : PmdRing(pmd_port, port_id, ring_id, ndesc) {}

  TxRing(const PmdPort *pmd_port, uint8_t port_id, uint16_t ring_id,
         uint16_t ndesc, struct rte_eth_txconf txconf)
      : PmdRing(pmd_port, port_id, ring_id, ndesc), conf_(txconf) {}

  TxRing(const PmdPort *pmd_port, uint8_t port_id, uint16_t ring_id,
         uint16_t ndesc, struct rte_eth_txconf txconf, uint32_t nmbufs,
         uint32_t mbuf_sz)
      : PmdRing(pmd_port, port_id, ring_id, ndesc, nmbufs, mbuf_sz),
        conf_(txconf) {}

  TxRing(TxRing const &) = delete;
  TxRing &operator=(TxRing const &) = delete;

  void Init();

  /**
   * @brief Tries to send a burst of packets through this TX ring.
   *
   * @param pkts Array of packet pointers to send.
   * @param nb_pkts Number of packets to send.
   * @return Number of packets successfully sent.
   */
  uint16_t TrySendPackets(Packet **pkts, uint16_t nb_pkts) const {
    const uint16_t nb_success =
        rte_eth_tx_burst(this->GetPortId(), this->GetRingId(),
                         reinterpret_cast<struct rte_mbuf **>(pkts), nb_pkts);

    // Free not-sent packets. TODO (ilias): This drops packets!
    for (auto i = nb_success; i < nb_pkts; ++i) Packet::Free(pkts[i]);
    return nb_success;
  }

  /**
   * @brief Tries to send a burst of packets through this TX ring.
   *
   * @param batch Batch of packets to send.
   */
  uint16_t TrySendPackets(PacketBatch *batch) const {
    const uint16_t ret = TrySendPackets(batch->pkts(), batch->GetSize());
    batch->Clear();
    return ret;
  }

  /**
   * @brief Sends all packets from a PacketBatch through this TX ring. Retries
   * until all are sent.
   *
   * @param pkts Array of packet pointers to send.
   * @param nb_pkts Number of packets to send.
   */
  void SendPackets(Packet **pkts, uint16_t nb_pkts) const {
    uint16_t nb_remaining = nb_pkts;

    do {
      auto index = nb_pkts - nb_remaining;
      auto nb_success = rte_eth_tx_burst(
          this->GetPortId(), this->GetRingId(),
          reinterpret_cast<struct rte_mbuf **>(&pkts[index]), nb_remaining);
      nb_remaining -= nb_success;
    } while (nb_remaining);
  }

  /**
   * @brief Sends all packets from a PacketBatch through this TX ring. Retries
   * until all are sent.
   *
   * @param batch Pointer to the PacketBatch.
   */
  void SendPackets(PacketBatch *batch) const {
    SendPackets(batch->pkts(), batch->GetSize());
    batch->Clear();
  }

  /**
   * @brief Explicitly reclaims the memory buffers (mbufs) used by sent packets
   * in the TX ring.
   */
  int ReclaimTxMbufs() const {
    return rte_eth_tx_done_cleanup(this->GetPortId(), this->GetRingId(), 0);
  }

 private:
  struct rte_eth_txconf conf_;
};

/**
 * @brief Represents a RX ring in DPDK.
 *
 * Provides functionalities specific to RX rings in DPDK.
 */
class RxRing : public PmdRing {
 public:
  RxRing(const PmdPort *pmd_port, uint8_t port_id, uint16_t ring_id,
         uint16_t ndesc)
      : PmdRing(pmd_port, port_id, ring_id, ndesc) {}

  RxRing(const PmdPort *pmd_port, uint8_t port_id, uint16_t ring_id,
         uint16_t ndesc, struct rte_eth_rxconf rxconf, uint32_t nmbufs,
         uint32_t mbuf_sz)
      : PmdRing(pmd_port, port_id, ring_id, ndesc, nmbufs, mbuf_sz),
        conf_(rxconf) {}

  RxRing(RxRing const &) = delete;
  RxRing &operator=(RxRing const &) = delete;

  void Init();

  /**
   * @brief Receives a burst of packets from this RX ring.
   *
   * This method fetches packets that have arrived on the RX ring
   * up to the number specified by nb_pkts. The received packets are
   * stored in the provided pkts array.
   *
   * @param pkts Array of packet pointers to store the received packets.
   * @param nb_pkts Maximum number of packets to receive.
   * @return Number of packets successfully received.
   */
  uint16_t RecvPackets(Packet **pkts, uint16_t nb_pkts) {
    return rte_eth_rx_burst(this->GetPortId(), this->GetRingId(),
                            reinterpret_cast<struct rte_mbuf **>(pkts),
                            nb_pkts);
  }

  /**
   * @brief Receives a burst of packets from this RX ring.
   *
   * This method fetches packets that have arrived on the RX ring
   * up to the number specified by nb_pkts. The received packets are
   * stored in the provided batch; the number of packets added is at most the
   * amount of available slots remaining in the batch.
   *
   * @param batch Batch of packets to store the received packets.
   * @return Number of packets successfully received.
   */
  uint16_t RecvPackets(PacketBatch *batch) {
    const uint16_t nb_rx = RecvPackets(batch->pkts(), batch->GetRoom());
    batch->IncrCount(nb_rx);
    return nb_rx;
  }

 private:
  struct rte_eth_rxconf conf_;
};

/**
 * @brief Factory function to create a ring.
 * @tparam T Type of the ring to be created.
 * @tparam Args Types of the arguments.
 * @param params Arguments for ring's constructor.
 * @return A unique pointer to the created ring.
 */
template <typename T, typename... Args>
decltype(auto) makeRing(Args &&...params) {
  std::unique_ptr<T> ptr(nullptr);
  ptr.reset(new T(std::forward<Args>(params)...));
  return ptr;
}

/**
 * @brief Represents a DPDK Port with functionalities to manage RX and TX rings.
 *
 * Provides a higher-level abstraction over a DPDK port, encapsulating
 * operations such as initialization, deinitialization, statistics gathering,
 * and other related functions.
 */
class PmdPort {
 public:
  static const uint16_t kDefaultRingNr_ = 1;

  /**
   * @brief Constructor to initialize a PmdPort with given parameters.
   *
   * @param id Port identifier.
   * @param rx_rings_nr (Optional) Number of RX rings. Default is
   * kDefaultRingNr_.
   * @param tx_rings_nr (Optional) Number of TX rings. Default is
   * kDefaultRingNr_.
   * @param rx_desc_nr (Optional) Number of RX descriptors. Default is
   * PmdRing::kDefaultRingDescNr.
   * @param tx_desc_nr (Optional) Number of TX descriptors. Default is
   * PmdRing::kDefaultRingDescNr.
   */
  PmdPort(uint16_t id, uint16_t rx_rings_nr = kDefaultRingNr_,
          uint16_t tx_rings_nr = kDefaultRingNr_,
          uint16_t rx_desc_nr = PmdRing::kDefaultRingDescNr,
          uint16_t tx_desc_nr = PmdRing::kDefaultRingDescNr)
      : is_dpdk_primary_process_(rte_eal_process_type() == RTE_PROC_PRIMARY),
        port_id_(id),
        tx_rings_nr_(tx_rings_nr),
        rx_rings_nr_(rx_rings_nr),
        tx_ring_desc_nr_(tx_desc_nr),
        rx_ring_desc_nr_(rx_desc_nr),
        initialized_(false) {
    // Get L2 address.
    rte_ether_addr temp;
    CHECK_EQ(rte_eth_macaddr_get(port_id_, &temp), 0);
    l2_addr_.FromUint8(temp.addr_bytes);
  }

  /**
   * @brief Deleted copy constructor and assignment operator.
   */
  PmdPort(PmdPort const &) = delete;
  PmdPort &operator=(PmdPort const &) = delete;

  /**
   * @brief Destructor. Deinitializes the port if it was initialized.
   */
  ~PmdPort() { DeInit(); }

  /**
   * @brief Initializes the driver.
   *
   * @param mtu (Optional) Maximum Transmission Unit to set for the port.
   * Default is PmdRing::kDefaultFrameSize.
   */
  void InitDriver(uint16_t mtu = PmdRing::kDefaultFrameSize);

  /**
   * @brief Deinitializes the port.
   */
  void DeInit();

  /**
   * @brief Checks if the port has been initialized.
   *
   * @return True if the port is initialized, false otherwise.
   */
  bool IsInitialized() const { return initialized_; }

  uint16_t GetPortId() const { return port_id_; }

  std::string GetDriverName() const {
    return juggler::utils::Format("%s", devinfo_.driver_name);
  }

  /**
   * @brief Retrieves the associated device for this port.
   *
   * @return Pointer to the associated device.
   */
  rte_device *GetDevice() const { return device_; }

  template <typename T>
  decltype(auto) GetRing(uint16_t id) const {
    constexpr bool is_tx_ring = std::is_same<T, TxRing>::value;
    CHECK_LT(id, (is_tx_ring ? tx_rings_nr_ : rx_rings_nr_)) << "Out-of-bounds";
    return is_tx_ring ? static_cast<T *>(tx_rings_.at(id).get())
                      : static_cast<T *>(rx_rings_.at(id).get());
  }

  /**
   * @brief Retrieves the Maximum Transmission Unit (MTU) for the port.
   *
   * @return An optional containing the MTU value if successful, or std::nullopt
   * if there was an error.
   */
  std::optional<uint16_t> GetMTU() const {
    uint16_t mtu;
    int ret = rte_eth_dev_get_mtu(port_id_, &mtu);
    if (ret != 0) return std::nullopt;  // Error (wrong port id?)
    return mtu;
  }

  /**
   * @brief Retrieves the MAC address of the port as set in the hardware.
   *
   * @return `Ethernet' address representing the MAC address.
   */
  net::Ethernet::Address GetL2Addr() const { return l2_addr_; }

  /**
   * @brief Retrieves the port's RSS (Receive Side Scaling) key.
   *
   * @return A vector containing the RSS hash key bytes.
   */
  const std::vector<uint8_t> &GetRSSKey() const { return rss_hash_key_; }

  /**
   * @brief Calculates the landing RX queue for a given RSS hash.
   *
   * @param rss_hash The RSS hash value.
   * @return The index of the RX queue.
   */
  uint16_t GetRSSRxQueue(uint32_t rss_hash) const {
    auto lsb = rss_hash & (devinfo_.reta_size - 1);
    auto index = lsb / RTE_ETH_RETA_GROUP_SIZE;
    auto shift = lsb % RTE_ETH_RETA_GROUP_SIZE;
    LOG(INFO) << "index: " << index << " shift: " << shift
              << "rss_hash: " << rss_hash
              << " reta_size: " << devinfo_.reta_size
              << " reta_group_size: " << RTE_ETH_RETA_GROUP_SIZE
              << " reta: " << rss_reta_conf_[index].reta[shift]
              << " lsb: " << lsb;
    return rss_reta_conf_[index].reta[shift];
  }

  /**
   * @brief Retrieves the number of RX queues for the port.
   *
   * @return Number of RX queues.
   */
  uint16_t GetRxQueuesNr() const { return rx_rings_nr_; }

  /**
   * @brief Updates the statistics for this port.
   */
  void UpdatePortStats();

  uint64_t GetPortRxPkts() const { return port_stats_.ipackets; }

  uint64_t GetPortRxBytes() const { return port_stats_.ibytes; }

  uint64_t GetPortTxPkts() const { return port_stats_.opackets; }

  uint64_t GetPortTxBytes() const { return port_stats_.obytes; }

  uint64_t GetPortRxDrops() const { return port_stats_.imissed; }

  uint64_t GetPortTxDrops() const { return port_stats_.oerrors; }

  uint64_t GetPortRxNoMbufErr() const { return port_stats_.rx_nombuf; }

  uint64_t GetPortQueueRxPkts(uint16_t queue_id) const {
    CHECK_LT(queue_id,
             std::min(rx_rings_.size(),
                      static_cast<size_t>(RTE_ETHDEV_QUEUE_STAT_CNTRS)));
    return port_stats_.q_ipackets[queue_id];
  }

  uint64_t GetPortQueueRxBytes(uint16_t queue_id) const {
    CHECK_LT(queue_id,
             std::min(rx_rings_.size(),
                      static_cast<size_t>(RTE_ETHDEV_QUEUE_STAT_CNTRS)));
    return port_stats_.q_ibytes[queue_id];
  }

  uint64_t GetPortQueueTxPkts(uint16_t queue_id) const {
    CHECK_LT(queue_id,
             std::min(tx_rings_.size(),
                      static_cast<size_t>(RTE_ETHDEV_QUEUE_STAT_CNTRS)));
    return port_stats_.q_opackets[queue_id];
  }

  uint64_t GetPortQueueTxBytes(uint16_t queue_id) const {
    CHECK_LT(queue_id,
             std::min(tx_rings_.size(),
                      static_cast<size_t>(RTE_ETHDEV_QUEUE_STAT_CNTRS)));
    return port_stats_.q_obytes[queue_id];
  }

  void DumpStats() {
    UpdatePortStats();
    LOG(INFO) << juggler::utils::Format(
        "[STATS - Port: %u] [TX] Pkts: %lu, Bytes: %lu, Drops: %lu [RX] Pkts: "
        "%lu, Bytes: %lu, Drops: %lu, NoRXMbufs: %lu",
        port_id_, GetPortTxPkts(), GetPortTxBytes(), GetPortTxDrops(),
        GetPortRxPkts(), GetPortRxBytes(), GetPortRxDrops(),
        GetPortRxNoMbufErr());

    for (uint16_t i = 0; i < tx_rings_nr_; i++) {
      LOG(INFO) << juggler::utils::Format(
          "[STATS - Port: %u, Queue: %u] [TX] Pkts: %lu, Bytes: %lu", port_id_,
          i, GetPortQueueTxPkts(i), GetPortQueueTxBytes(i));
    }

    for (uint16_t i = 0; i < rx_rings_nr_; i++) {
      LOG(INFO) << juggler::utils::Format(
          "[STATS - Port: %u, Queue: %u] [RX] Pkts: %lu, Bytes: %lu", port_id_,
          i, GetPortQueueRxPkts(i), GetPortQueueRxBytes(i));
    }
  }

 private:
  const bool is_dpdk_primary_process_;
  const uint16_t port_id_;
  const uint16_t tx_rings_nr_, rx_rings_nr_;
  uint16_t tx_ring_desc_nr_, rx_ring_desc_nr_;
  std::vector<std::unique_ptr<PmdRing>> tx_rings_, rx_rings_;

  juggler::net::Ethernet::Address l2_addr_;
  struct rte_eth_dev_info devinfo_;
  rte_device *device_;
  std::vector<rte_eth_rss_reta_entry64> rss_reta_conf_;
  struct rte_eth_stats port_stats_;
  std::vector<uint8_t> rss_hash_key_;
  bool initialized_;
};
}  // namespace dpdk
}  // namespace juggler

#endif  // SRC_INCLUDE_PMD_H_
