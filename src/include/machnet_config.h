#ifndef SRC_INCLUDE_MACHNET_CONFIG_H_
#define SRC_INCLUDE_MACHNET_CONFIG_H_

#include <glog/logging.h>

#include <fstream>
#define JSON_NOEXCEPTION  // Disable exceptions for nlohmann::json
#include <dpdk.h>
#include <ether.h>
#include <ipv4.h>
#include <utils.h>

#include <algorithm>
#include <nlohmann/json.hpp>
#include <unordered_set>

namespace juggler {

class NetworkInterfaceConfig {
 public:
  inline static const cpu_set_t kDefaultCpuMask =
      utils::calculate_cpu_mask(0xFFFFFFFF);
  explicit NetworkInterfaceConfig(const std::string pcie_addr,
                                  const net::Ethernet::Address &l2_addr,
                                  const net::Ipv4::Address &ip_addr,
                                  size_t engine_threads = 1,
                                  cpu_set_t cpu_mask = kDefaultCpuMask)
      : pcie_addr_(pcie_addr),
        l2_addr_(l2_addr),
        ip_addr_(ip_addr),
        engine_threads_(engine_threads),
        cpu_mask_(cpu_mask),
        dpdk_port_id_(std::nullopt) {}
  bool operator==(const NetworkInterfaceConfig &other) const {
    return l2_addr_ == other.l2_addr_;
  }

  const std::string &pcie_addr() const { return pcie_addr_; }
  const net::Ethernet::Address &l2_addr() const { return l2_addr_; }
  const net::Ipv4::Address &ip_addr() const { return ip_addr_; }
  size_t engine_threads() const { return engine_threads_; }
  cpu_set_t cpu_mask() const { return cpu_mask_; }
  std::optional<uint16_t> dpdk_port_id() const { return dpdk_port_id_; }
  void Dump() const {
    LOG(INFO) << "NetworkInterfaceConfig: "
              << utils::Format(
                     "[PCIe: %s, L2: %s, IP: %s, engine_threads: %zu, "
                     "cpu_mask: %lu, dpdk_port_id: %d]",
                     pcie_addr_.c_str(), l2_addr_.ToString().c_str(),
                     ip_addr_.ToString().c_str(), engine_threads_,
                     utils::cpuset_to_sizet(cpu_mask_),
                     dpdk_port_id_.value_or(-1));
  }

  void set_dpdk_port_id(std::optional<uint16_t> dpdk_port_id) {
    dpdk_port_id_ = dpdk_port_id;
  }

 private:
  const std::string pcie_addr_;
  const net::Ethernet::Address l2_addr_;
  const net::Ipv4::Address ip_addr_;
  const size_t engine_threads_;
  cpu_set_t cpu_mask_;
  std::optional<uint16_t> dpdk_port_id_;
};
}  // namespace juggler

namespace std {
template <>
struct hash<juggler::NetworkInterfaceConfig> {
  size_t operator()(const juggler::NetworkInterfaceConfig &nic) const {
    return std::hash<juggler::net::Ethernet::Address>()(nic.l2_addr());
  }
};
}  // namespace std

namespace juggler {
/**
 * @brief This is Machnet configuration JSON reader. The following is an example
 * of Machnet config on a single host:
 *
 * {
 *   "machnet_config": {
 *     "00:0d:3a:d6:9b:6a": {
 *         "ip": "10.0.0.1",
 *         "engine_threads": "1",
 *         "cpu_mask": "0x1"
 *     },
 *   }
 * }
 *
 * Each item in the "machnet_config" dictionary is a network interface MAC
 * address. The MAC address is the only reliable way to identify a network
 * interface especially on Azure.
 *
 * Note that `engine_threads` (decimal) and `cpu_mask` (hex) are optional. If
 * not specified, the default value is 1 and 0xFFFFFFFF respectively.
 */
class MachnetConfigProcessor {
 public:
  explicit MachnetConfigProcessor(const std::string &config_json_filename);

  std::unordered_set<NetworkInterfaceConfig> &interfaces_config() {
    return interfaces_config_;
  }
  utils::CmdLineOpts GetEalOpts() const;

 private:
  void AssertJsonValidMachnetConfig();
  void DiscoverInterfaceConfiguration();

  static constexpr const char *kMachnetConfigJsonKey = "machnet_config";
  const std::string config_json_filename_;
  std::unordered_set<NetworkInterfaceConfig> interfaces_config_;
  nlohmann::json json_;
};

}  // namespace juggler

#endif  // SRC_INCLUDE_MACHNET_CONFIG_H_
