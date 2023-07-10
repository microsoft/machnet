// Code originally written for BESS, modified here.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// Copyright (c) 2017, Cloudigo.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef SRC_INCLUDE_ETHER_H_
#define SRC_INCLUDE_ETHER_H_

#include <types.h>
#include <utils.h>

#include <cstdint>
#include <string>

namespace juggler {
namespace net {

struct __attribute__((packed)) Ethernet {
  struct __attribute__((packed)) Address {
    static const uint8_t kSize = 6;
    Address() = default;
    Address(const uint8_t *addr) {
      bytes[0] = addr[0];
      bytes[1] = addr[1];
      bytes[2] = addr[2];
      bytes[3] = addr[3];
      bytes[4] = addr[4];
      bytes[5] = addr[5];
    }

    Address(const std::string mac_addr) { FromString(mac_addr); }

    void FromUint8(const uint8_t *addr) {
      bytes[0] = addr[0];
      bytes[1] = addr[1];
      bytes[2] = addr[2];
      bytes[3] = addr[3];
      bytes[4] = addr[4];
      bytes[5] = addr[5];
    }

    bool FromString(std::string addr);
    std::string ToString() const;

    Address &operator=(const Address &rhs) {
      bytes[0] = rhs.bytes[0];
      bytes[1] = rhs.bytes[1];
      bytes[2] = rhs.bytes[2];
      bytes[3] = rhs.bytes[3];
      bytes[4] = rhs.bytes[4];
      bytes[5] = rhs.bytes[5];
      return *this;
    }
    bool operator==(const Address &rhs) const {
      return bytes[0] == rhs.bytes[0] && bytes[1] == rhs.bytes[1] &&
             bytes[2] == rhs.bytes[2] && bytes[3] == rhs.bytes[3] &&
             bytes[4] == rhs.bytes[4] && bytes[5] == rhs.bytes[5];
    }
    bool operator!=(const Address &rhs) const { return !operator==(rhs); }

    uint8_t bytes[kSize];
  };
  inline static const Address kBroadcastAddr{"ff:ff:ff:ff:ff:ff"};
  inline static const Address kZeroAddr{"00:00:00:00:00:00"};

  enum EthType : uint16_t {
    kArp = 0x806,
    kIpv4 = 0x800,
    kIpv6 = 0x86DD,
  };

  std::string ToString() const;

  Address dst_addr;
  Address src_addr;
  be16_t eth_type;
};

}  // namespace net
}  // namespace juggler

namespace std {
template <>
struct hash<juggler::net::Ethernet::Address> {
  size_t operator()(const juggler::net::Ethernet::Address &addr) const {
    return juggler::utils::hash<uint32_t>(
        reinterpret_cast<const char *>(addr.bytes),
        juggler::net::Ethernet::Address::kSize);
  }
};
}  // namespace std

#endif  // SRC_INCLUDE_ETHER_H_
