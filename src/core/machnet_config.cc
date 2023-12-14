#include "machnet_config.h"

#include <algorithm>
#include <filesystem>
#include <ranges>
#include <string>

#include "dpdk.h"
#include "ether.h"

namespace juggler {

static std::optional<std::string> GetPCIeAddressSysfs(
    const juggler::net::Ethernet::Address &l2_addr) {
  // Note: This works on Azure even after we unbind the NIC (e.g., `eth1`)
  // because a "fake" sibling interface with the same MAC and PCIe address
  // remains in sysfs.
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

  LOG(WARNING) << "Failed to get PCI address for L2 address "
               << l2_addr.ToString() << " by the sysfs method.";
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
      if (key != "ip" && key != "engine_threads" && key != "cpu_mask" &&
          key != "pcie") {
        LOG(FATAL) << "Invalid key " << key << " in " << interface << " in "
                   << config_json_filename_;
      }
    }
  }
}

void MachnetConfigProcessor::DiscoverInterfaceConfiguration() {
  for (const auto &[key, json_val] : json_.at(kMachnetConfigJsonKey).items()) {
    const net::Ethernet::Address l2_addr(key);
    size_t engine_threads = 1;
    cpu_set_t cpu_mask = NetworkInterfaceConfig::kDefaultCpuMask;

    net::Ipv4::Address ip_addr;
    CHECK(ip_addr.FromString(json_val.at("ip")));

    if (json_val.find("engine_threads") != json_val.end()) {
      engine_threads = json_val.at("engine_threads");
      LOG(INFO) << "Using " << engine_threads << " engine threads for "
                << l2_addr.ToString();
    } else {
      LOG(INFO) << "Using default engine threads = " << engine_threads
                << " for " << l2_addr.ToString();
    }

    if (json_val.find("cpu_mask") != json_val.end()) {
      std::string cpu_mask_str = json_val.at("cpu_mask");
      const size_t cpu_mask_val =
          std::stoull(cpu_mask_str.c_str(), nullptr, 16);
      cpu_mask = utils::calculate_cpu_mask(cpu_mask_val);
      LOG(INFO) << "Using CPU mask " << cpu_mask_str << " for "
                << l2_addr.ToString();
    } else {
      LOG(INFO) << "Using default CPU mask for " << l2_addr.ToString();
    }

    std::string pci_addr = "";
    if (json_val.find("pcie") != json_val.end()) {
      pci_addr = json_val.at("pcie");
      LOG(INFO) << "Using config file PCIe address " << pci_addr << " for "
                << l2_addr.ToString();
    } else {
      const std::optional<std::string> ret = GetPCIeAddressSysfs(l2_addr);
      if (ret.has_value()) {
        pci_addr = ret.value();
      } else {
        LOG(WARNING) << "Failed to get PCIe address from sysfs for L2 address "
                     << l2_addr.ToString()
                     << ", and no PCIe address specified in config file.";
      }
    }

    interfaces_config_.emplace(pci_addr, l2_addr, ip_addr, engine_threads,
                               cpu_mask);
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
    if (interface.pcie_addr() != "") {
      eal_opts.Append({"-a", interface.pcie_addr()});
    } else {
      LOG(WARNING) << "Not passing PCIe allowlist for interface "
                   << interface.l2_addr().ToString();
    }
  }

  return eal_opts;
}

}  // namespace juggler
