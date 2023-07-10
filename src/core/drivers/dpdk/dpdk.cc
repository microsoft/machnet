#include <dpdk.h>
#include <glog/logging.h>
#include <rte_common.h>
#include <rte_config.h>
#include <rte_cycles.h>
#include <rte_eal.h>

namespace juggler {
namespace dpdk {

void Dpdk::InitDpdk(juggler::utils::CmdLineOpts rte_args) {
  if (initialized_) {
    LOG(WARNING) << "DPDK is already initialized.";
    return;
  }

  LOG(INFO) << "Initializing DPDK with args: " << rte_args.ToString();
  int ret = rte_eal_init(rte_args.GetArgc(), rte_args.GetArgv());
  if (ret < 0) {
    LOG(FATAL) << "rte_eal_init() failed: ret = " << ret
               << " rte_errno = " << rte_errno << " ("
               << rte_strerror(rte_errno) << ")";
  }

  // Check if DPDK runs in PA or VA mode.
  if (rte_eal_iova_mode() == RTE_IOVA_VA) {
    LOG(INFO) << "DPDK runs in VA mode.";
  } else {
    LOG(INFO) << "DPDK runs in PA mode.";
  }

  ScanDpdkPorts();
  initialized_ = true;
}

void Dpdk::DeInitDpdk() {
  int ret = rte_eal_cleanup();
  if (ret != 0) {
    LOG(FATAL) << "rte_eal_cleanup() failed: ret = " << ret
               << " rte_errno = " << rte_errno << " ("
               << rte_strerror(rte_errno) << ")";
  }

  initialized_ = false;
}

size_t Dpdk::GetNumPmdPortsAvailable() { return rte_eth_dev_count_avail(); }

std::optional<uint16_t> Dpdk::GetPmdPortIdByMac(
    const juggler::net::Ethernet::Address &l2_addr) const {
  if (!initialized_) {
    LOG(WARNING) << "DPDK is not initialized. Cannot retrieve eth device "
                    "contextual info.";
    return std::nullopt;
  }

  std::optional<uint16_t> p_id = std::nullopt;
  uint16_t port_id;
  RTE_ETH_FOREACH_DEV(port_id) {
    std::string pci_info;
    juggler::net::Ethernet::Address lladdr;

    int ret = rte_eth_macaddr_get(
        port_id, reinterpret_cast<rte_ether_addr *>(lladdr.bytes));
    if (ret != 0) {
      LOG(WARNING)
          << "rte_eth_macaddr_get() failed. Cannot retrieve eth device "
             "contextual info for port "
          << static_cast<int>(port_id);
      break;
    }
    LOG(INFO) << "looking for " << l2_addr.ToString() << " found "
              << lladdr.ToString() << " port " << static_cast<int>(port_id);

    if (lladdr == l2_addr) {
      p_id = port_id;
    }
  }

  return p_id;
}

}  // namespace dpdk
}  // namespace juggler
