# TCP Flow Unit Tests

## What

`tcp_flow_test.cc` is a comprehensive unit test suite for the `TcpFlow` class (`src/include/tcp_flow.h`), which implements Machnet's TCP transport path. The suite contains **50 tests** covering the full TCP state machine, data path, and edge cases that arise when interoperating with the Linux kernel's TCP stack.

## Why

We added TCP support to Machnet alongside the existing UDP-based Machnet protocol. The `TcpFlow` class is a complex piece of code — it implements a TCP state machine, message framing/deframing over a byte stream, MSS negotiation, retransmission overlap handling, and interfaces with the shared-memory channel system. Any regression in this code could silently corrupt data or hang connections.

These tests exist to:

1. **Catch regressions** — the TCP code touches many subsystems (packet construction, channel MsgBuf allocation, sequence number tracking). A change in any of them could break TCP without affecting UDP.
2. **Validate bug fixes** — several bugs were found and fixed during development (Ethernet padding causing wrong payload length, IP `total_length` vs `packet->length()` mismatch, invalid TCP header lengths). Each fix has a corresponding test to prevent re-introduction.
3. **Document behavior** — the tests serve as executable documentation of how the TCP state machine behaves in specific scenarios (retransmitted SYNs in ESTABLISHED, piggybacked data on handshake ACK, partial retransmission overlaps).

## Test Categories

| Category | # Tests | Description |
|---|---|---|
| Sequence number helpers | 1 | Verifies wrap-around-safe comparisons (`SeqLt`, `SeqGt`, etc.) including the `0xFFFFFFFF → 0` boundary |
| Construction & initial state | 1 | Checks default values: state=CLOSED, ISN initialization, default MSS |
| Active open (client handshake) | 3 | `InitiateHandshake` → SYN_SENT, full 3-way handshake with callback, wrong-ACK rejection |
| Passive open (server handshake) | 4 | `StartPassiveOpen` → LISTEN, full handshake, MSS option parsing from SYN, piggybacked data on completing ACK |
| RST handling | 2 | RST received in ESTABLISHED and SYN_SENT states |
| TCP option parsing | 4 | MSS-only option, NOP-padded options, no options (default MSS), zero-MSS fallback |
| Header validation | 1 | Rejects TCP headers with length < 20 bytes |
| TX path (OutputMessage) | 3 | Small message framing, not-established guard, multi-segment when payload > MSS |
| RX deframing (ConsumePayload) | 3 | Single complete message, length prefix split across calls, two back-to-back messages |
| Established data handling | 2 | Data packet delivery to channel, retransmitted SYN re-ACK in ESTABLISHED |
| Retransmission overlap | 4 | Exact in-order delivery, partial overlap (kernel retransmit), pure duplicate, empty payload |
| FIN handling | 3 | Clean FIN → CLOSE_WAIT, FIN with trailing data, retransmitted FIN |
| Shutdown (active close) | 3 | From ESTABLISHED → FIN_WAIT_1, CLOSE_WAIT → LAST_ACK, SYN_SENT → RST + CLOSED |
| Graceful close (full teardown) | 2 | FIN_WAIT_1 → FIN_WAIT_2 → TIME_WAIT, passive close via LAST_ACK → CLOSED |
| PeriodicCheck (timers) | 4 | CLOSED returns false, TIME_WAIT countdown, SYN retransmit on RTO, max retransmissions exceeded |
| AdvanceSndUna | 4 | Normal ACK advancement, all-data-acked (RTO disarm), stale ACK ignored, future ACK ignored |
| StateToString | 1 | All 10 TCP states map to correct strings |
| Match | 2 | Correct 4-tuple match, wrong source address rejection |
| Ethernet padding resilience | 1 | Verifies IP `total_length` is used (not `packet->length()`) for payload size |
| IP length validation | 1 | Rejects packets where IP `total_length` < IP header + TCP header |

## How to Build and Run

```bash
cd build
cmake ..          # Only needed once or after adding new test files
ninja -j$(nproc) tcp_flow_test
sudo ./src/tests/tcp_flow_test
```

`sudo` is required because DPDK needs access to hugepages. The test uses `--vdev=net_null0,copy=1 --no-pci` so no physical NIC is needed.

## Test Infrastructure

- **Framework**: Google Test (gtest)
- **DPDK**: Initialized with a `net_null` virtual device — no hardware required
- **White-box testing**: Uses `#define private public` (applied only to `tcp_flow.h`) to access internal state for assertions
- **Fixture**: `TcpFlowTest` with a static `PmdPort`/`TxRing` (shared across tests to avoid DPDK port teardown issues) and per-test `Channel` + `PacketPool`
- **Packet crafting**: `MakePacket()` helper builds Ethernet + IPv4 + TCP packets with correct headers, optional TCP options, and optional payload
- **Auto-discovery**: CMake globs `*_test.cc` under `src/`, so the test is automatically picked up without modifying `CMakeLists.txt`
