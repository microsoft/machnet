#ifndef SRC_APPS_ECHO_CONFIG_JSON_READER_H_
#define SRC_APPS_ECHO_CONFIG_JSON_READER_H_

#include <glog/logging.h>

#include <fstream>
#define JSON_NOEXCEPTION  // Disable exceptions for nlohmann::json
#include <nlohmann/json.hpp>

#include "ipv4.h"

/**
 * @brief For convenience, we store information about the NIC interfaces
 * use for Juggler communication in a json file. This class provides a
 * convenience interface to fetch this information. The json has entries of the
 * form:
 *
 * {
 *   "host_config": {
 *     "roce93": {
 *         "pcie_allowlist": "12:00.0",
 *         "ipv4_addr": "192.168.23.193",
 *         "mac_addr": "ec:0d:9a:e0:f6:d0",
 *         "eal_args": "-l 0-2 -n 4 --log-level=eal,8" # Optional key.
 *     }
 *   }
 * }
 */
class JugglerConfigJsonReader {
 public:
  explicit JugglerConfigJsonReader(const std::string &config_json_filename)
      : config_json_filename_(config_json_filename) {
    std::ifstream config_json_file(config_json_filename);
    CHECK(config_json_file.is_open())
        << " Failed to open config JSON file " << config_json_filename << ".";

    config_json_file >> json_;
  }

  std::string GetAllConfigsForHost(const std::string &hostname) const {
    AssertJsonContainsHost(hostname);
    return json_.at(kHostsConfigJsonKey).at(hostname).dump();
  }

  std::string GetPcieAllowlistForHost(const std::string &hostname) const {
    AssertJsonContainsKeyForHost(hostname, kPcieAllowlistJsonKey);
    return json_.at(kHostsConfigJsonKey).at(hostname).at(kPcieAllowlistJsonKey);
  }

  int GetPmdPortIdForHost(const std::string &hostname) const {
    AssertJsonContainsKeyForHost(hostname, kPmdPortIdJsonKey);
    return json_.at(kHostsConfigJsonKey).at(hostname).at(kPmdPortIdJsonKey);
  }

  std::string GetIPv4AddressForHost(const std::string &hostname) const {
    AssertJsonContainsKeyForHost(hostname, kIPv4AddrJsonKey);
    const std::string ipv4_str =
        json_.at(kHostsConfigJsonKey).at(hostname).at("ipv4_addr");
    if (!juggler::net::Ipv4::Address::IsValid(ipv4_str)) {
      LOG(FATAL) << "Malformed IPv4 address " << ipv4_str << " for host "
                 << hostname << " in " << config_json_filename_;
    }
    return ipv4_str;
  }

  std::string GetMACAddressForHost(const std::string &hostname) const {
    AssertJsonContainsKeyForHost(hostname, kMacAddrJsonKey);
    return json_.at(kHostsConfigJsonKey).at(hostname).at(kMacAddrJsonKey);
  }

  std::string GetEalArgsForHost(const std::string &hostname) const {
    AssertJsonContainsHost(hostname);
    const nlohmann::json &host_entry =
        json_.at(kHostsConfigJsonKey).at(hostname);

    // If the key is not found in configuration, return empty string.
    if (host_entry.find(kEalArgsKey) == host_entry.end()) {
      return "";
    }

    return json_.at(kHostsConfigJsonKey).at(hostname).at(kEalArgsKey);
  }

  void AssertJsonContainsHost(const std::string &hostname) const {
    if (json_.find(kHostsConfigJsonKey) == json_.end()) {
      LOG(FATAL) << "No entry for hosts config (key " << kHostsConfigJsonKey
                 << ") in " << config_json_filename_;
    }

    const nlohmann::json &host_config_entry = json_.at(kHostsConfigJsonKey);
    if (host_config_entry.find(hostname) == host_config_entry.end()) {
      LOG(FATAL) << "No entry for host " << hostname << " in "
                 << config_json_filename_;
    }
  }

  void AssertJsonContainsKeyForHost(const std::string &hostname,
                                    const std::string &key) const {
    AssertJsonContainsHost(hostname);

    const nlohmann::json &host_entry =
        json_.at(kHostsConfigJsonKey).at(hostname);
    if (host_entry.find(key) == host_entry.end()) {
      LOG(FATAL) << "No entry for key " << key << " for host " << hostname
                 << " in " << config_json_filename_;
    }
  }

 private:
  static constexpr const char *kPcieAllowlistJsonKey = "pcie_allowlist";
  static constexpr const char *kIPv4AddrJsonKey = "ipv4_addr";
  static constexpr const char *kMacAddrJsonKey = "mac_addr";
  static constexpr const char *kPmdPortIdJsonKey = "pmd_port_id";
  static constexpr const char *kEalArgsKey = "eal_args";
  static constexpr const char *kHostsConfigJsonKey = "hosts_config";

  const std::string config_json_filename_;
  nlohmann::json json_;
};

#endif  // SRC_APPS_ECHO_CONFIG_JSON_READER_H_
