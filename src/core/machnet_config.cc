#include "machnet_config.h"

#include <algorithm>
#include <filesystem>
#include <ranges>
#include <string>

#include "dpdk.h"
#include "ether.h"

namespace juggler {

static std::optional<std::string> GetPCIeAddress(
    const juggler::net::Ethernet::Address &l2_addr) {
  LOG(INFO) << "Walking /sys/class/net to find PCIe address for L2 address "
            << l2_addr.ToString();

  std::string sys_net_path = "/sys/class/net";
  for (const auto &entry : std::filesystem::directory_iterator(sys_net_path)) {
    std::string interface_name = entry.path().filename();
    LOG(INFO) << "Checking interface " << interface_name;

    // Check the address file (contains the MAC) for this path.
    std::string address_fname = entry.path() / "address";
    if (!std::filesystem::exists(address_fname)) {
      continue;
    }

    std::ifstream address_file(address_fname);
    // Get the interface's MAC address from the address file.
    std::string interface_l2_addr;
    address_file >> interface_l2_addr;

    const juggler::net::Ethernet::Address interface_addr(interface_l2_addr);
    if (interface_addr == l2_addr) {
      // Get the PCI address of the interface.
      std::string dev_uevent_fname = entry.path() / "device" / "uevent";
      if (!std::filesystem::exists(dev_uevent_fname)) {
        continue;
      }
      std::ifstream uevent_file(dev_uevent_fname);
      // Find the PCI address in the uevent file. It is a single line in the
      // form:
      // PCI_SLOT_NAME=0000:00:1f.0
      std::string pcie_addr;
      while (uevent_file >> pcie_addr) {
        if (pcie_addr.find("PCI_SLOT_NAME") != std::string::npos) {
          return pcie_addr.substr(pcie_addr.find("=") + 1);
        }
      }
    }
  }
  return std::nullopt;
}

MachnetConfigProcessor::MachnetConfigProcessor(
    const std::string &config_json_filename)
    : config_json_filename_(config_json_filename), interfaces_config_{} {
  std::ifstream config_json_file(config_json_filename);
  CHECK(config_json_file.is_open())
      << " Failed to open config JSON file " << config_json_filename << ".";

  config_json_file >> json_;
  AssertJsonValidMachnetConfig();
  DiscoverInterfaceConfiguration();
}

void MachnetConfigProcessor::AssertJsonValidMachnetConfig() {
  if (json_.find(kMachnetConfigJsonKey) == json_.end()) {
    LOG(FATAL) << "No entry for Machnet config (key " << kMachnetConfigJsonKey
               << ") in " << config_json_filename_;
  }

  for (const auto &interface : json_.at(kMachnetConfigJsonKey)) {
    if (interface.find("ip") == interface.end()) {
      LOG(FATAL) << "No IP address for " << interface << " in "
                 << config_json_filename_;
    }
    for (const auto &[key, _] : interface.items()) {
      if (key != "ip" && key != "engine_threads" && key != "cpu_mask") {
        LOG(FATAL) << "Invalid key " << key << " in " << interface << " in "
                   << config_json_filename_;
      }
    }
  }
}

void MachnetConfigProcessor::DiscoverInterfaceConfiguration() {
  for (const auto &[key, val] : json_.at(kMachnetConfigJsonKey).items()) {
    const net::Ethernet::Address l2_addr(key);
    size_t engine_threads = 1;
    cpu_set_t cpu_mask = NetworkInterfaceConfig::kDefaultCpuMask;

    net::Ipv4::Address ip_addr;
    CHECK(ip_addr.FromString(val.at("ip")));

    if (val.find("engine_threads") != val.end()) {
      engine_threads = val.at("engine_threads");
    }

    if (val.find("cpu_mask") != val.end()) {
      std::string cpu_mask_str = val.at("cpu_mask");
      const size_t cpu_mask_val =
          std::stoull(cpu_mask_str.c_str(), nullptr, 16);
      cpu_mask = utils::calculate_cpu_mask(cpu_mask_val);
    }

    // Get PCIe address of the interface for this mac address.
    auto pci_addr = GetPCIeAddress(l2_addr);
    if (!pci_addr) {
      LOG(FATAL) << "Failed to find PCI address for interface with MAC "
                 << l2_addr.ToString() << ".";
    }

    interfaces_config_.emplace(pci_addr.value(), l2_addr, ip_addr,
                               engine_threads, cpu_mask);
  }
  for (const auto &interface : interfaces_config_) {
    interface.Dump();
  }
}

utils::CmdLineOpts MachnetConfigProcessor::GetEalOpts() const {
  utils::CmdLineOpts eal_opts{juggler::dpdk::kDefaultEalOpts};
  // TODO(ilias) : What cpu mask to set for EAL?
  eal_opts.Append({"-c", "0x1"});
  eal_opts.Append({"-n", "4"});
  for (const auto &interface : interfaces_config_) {
    eal_opts.Append({"-a", interface.pcie_addr()});
  }

  return eal_opts;
}

}  // namespace juggler
