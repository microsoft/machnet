# Machnet benchmarks

## Single-connection request-response benchmark

Description: A client sends a request to a server, and the server sends a
response back to the client. The client keeps a configurable number messages in
flight for pipelining.

### Start the Machnet service on both client and server

```bash
./machnet.sh --mac <server_DPDK_IP> --ip <server_DPDK_IP>
```

Commands used for `msg_gen`:
```bash
# Server
./msg_gen --local_ip <server_DPDK_IP> --msg_size 1024

# Client: Experiment #1: latency
./msg_gen --local_ip <client DPDK IP> --remote_ip <server DPDK IP> --active_generator --msg_window 1 --msg_size 1024

# Client: Experiment #2: message rate
./msg_gen --local_ip <client DPDK IP> --remote_ip <server DPDK IP> --active_generator --msg_window 32 --msg_size 1024
```

| Server | NIC type | Experiment | Round-trip p50, p99, p99.9 | Request rate | Date |
| --- | --- | --- | --- | --- | --- |
| Azure F8s_v2, Ubuntu 22.04 |  Connect4-Lx  | Latency | XXX | XXX | Dec 2023
| |  | Message rate | XXX | XXX | Dec 2023
| --- | --- | --- | --- | --- | --- |
| Bare metal, CentOS | E810 PH | Message rate | 35 us, 45 us, 51 us | 218K/sec | Dec 2023




