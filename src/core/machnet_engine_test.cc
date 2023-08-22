/**
 * @file machnet_engine_test.cc
 *
 * Unit tests for the MachnetEngine class.
 */

#include <channel.h>
#include <dpdk.h>
#include <gtest/gtest.h>
#include <machnet_engine.h>
#include <packet.h>
#include <pmd.h>

#include <memory>
#include <numeric>

constexpr const char *file_name(const char *path) {
  const char *file = path;
  while (*path) {
    if (*path++ == '/') {
      file = path;
    }
  }
  return file;
}

const char *fname = file_name(__FILE__);

TEST(BasicMachnetEngineSharedStateTest, SrcPortAlloc) {
  using EthAddr = juggler::net::Ethernet::Address;
  using Ipv4Addr = juggler::net::Ipv4::Address;
  using UdpPort = juggler::net::Udp::Port;
  using MachnetEngineSharedState = juggler::MachnetEngineSharedState;

  EthAddr test_mac{"00:00:00:00:00:01"};
  Ipv4Addr test_ip;
  test_ip.FromString("10.0.0.1");

  MachnetEngineSharedState state({}, {test_mac}, {test_ip});
  std::vector<UdpPort> allocated_ports;
  do {
    auto port = state.SrcPortAlloc(test_ip, [](uint16_t port) { return true; });
    if (!port.has_value()) break;
    allocated_ports.emplace_back(port.value());
  } while (true);

  std::vector<UdpPort> expected_ports;
  expected_ports.resize(MachnetEngineSharedState::kSrcPortMax -
                        MachnetEngineSharedState::kSrcPortMin + 1);
  std::iota(expected_ports.begin(), expected_ports.end(),
            MachnetEngineSharedState::kSrcPortMin);

  EXPECT_EQ(allocated_ports, expected_ports);

  auto release_allocated_ports = [&state,
                                  &test_ip](std::vector<UdpPort> &ports) {
    while (!ports.empty()) {
      state.SrcPortRelease(test_ip, ports.back());
      ports.pop_back();
    }
  };
  release_allocated_ports(allocated_ports);

  // Test whether the lambda condition for port allocation works.
  // Try to allocate all ports divisible by 3.
  auto is_divisible_by_3 = [](uint16_t port) { return port % 3 == 0; };
  do {
    auto port = state.SrcPortAlloc(test_ip, is_divisible_by_3);
    if (!port.has_value()) break;
    allocated_ports.emplace_back(port.value());
  } while (true);

  expected_ports.clear();
  for (size_t p = MachnetEngineSharedState::kSrcPortMin;
       p <= MachnetEngineSharedState::kSrcPortMax; p++) {
    if (is_divisible_by_3(p)) {
      expected_ports.emplace_back(p);
    }
  }

  EXPECT_EQ(allocated_ports, expected_ports);
  release_allocated_ports(allocated_ports);

  auto illegal_condition = [](uint16_t port) { return port == 0; };
  auto port = state.SrcPortAlloc(test_ip, illegal_condition);
  EXPECT_FALSE(port.has_value());
}

TEST(BasicMachnetEngineTest, BasicMachnetEngineTest) {
  using PmdPort = juggler::dpdk::PmdPort;
  using MachnetEngine = juggler::MachnetEngine;

  const uint32_t kChannelRingSize = 1024;
  juggler::shm::ChannelManager channel_mgr;
  channel_mgr.AddChannel(fname, kChannelRingSize, kChannelRingSize,
                         kChannelRingSize, kChannelRingSize);
  auto channel = channel_mgr.GetChannel(fname);

  juggler::net::Ethernet::Address test_mac("00:00:00:00:00:01");
  juggler::net::Ipv4::Address test_ip;
  test_ip.FromString("10.0.0.1");
  std::vector<uint8_t> rss_key = {};
  std::vector<juggler::net::Ipv4::Address> test_ips = {test_ip};
  auto shared_state = std::make_shared<juggler::MachnetEngineSharedState>(
      rss_key, test_mac, test_ips);
  const uint32_t kRingDescNr = 1024;
  auto pmd_port = std::make_shared<PmdPort>(0, 1, 1, kRingDescNr, kRingDescNr);
  pmd_port->InitDriver();
  MachnetEngine engine(pmd_port, 0, 0, shared_state, {channel});
  EXPECT_EQ(engine.GetChannelCount(), 1);
}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);

  auto kEalOpts = juggler::utils::CmdLineOpts(
      {"-c", "0x0", "-n", "6", "--proc-type=auto", "-m", "1024", "--log-level",
       "8", "--vdev=net_null0,copy=1", "--no-pci"});

  auto d = juggler::dpdk::Dpdk();
  d.InitDpdk(kEalOpts);
  int ret = RUN_ALL_TESTS();
  return ret;
}
