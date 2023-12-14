# Machnet benchmarks

## Single-connection request-response benchmark

Description: A client sends a request to a server, and the server sends a
response back to the client. The client keeps a configurable number messages in
flight for pipelining.

Start the Machnet Docker container on both client and server

```bash
./machnet.sh --mac <server_DPDK_IP> --ip <server_DPDK_IP>
```

Run the `msg_gen` benchmark
```bash
# Server
./msg_gen --local_ip <server_DPDK_IP> --msg_size 1024

# Client: Experiment #1: latency
./msg_gen --local_ip <client DPDK IP> --remote_ip <server DPDK IP> --active_generator --msg_window 1 --msg_size 1024

# Client: Experiment #2: message rate
./msg_gen --local_ip <client DPDK IP> --remote_ip <server DPDK IP> --active_generator --msg_window 32 --msg_size 1024
```

| Server | NIC type | Experiment | Round-trip p50 99 99.9 | Request rate | Date |
| --- | --- | --- | --- | --- | --- |
| Azure F8s_v2, Ubuntu 22.04 |  Connect4-Lx  | Latency | XXX | XXX | Dec 2023
| |  | Message rate | XXX | XXX | Dec 2023
| EC2 XXX, XXX | ENA | Latency | XXX | XXX | Dec 2023
| |  | Message rate | XXX | XXX | Dec 2023
| GCP XXX, XXX | gVNIC | Latency | XXX | XXX | Dec 2023
| |  | Message rate | XXX | XXX | Dec 2023
| Bare metal, Mariner | E810 PF | Latency | XXX | XXX | Dec 2023
| |  | Message rate | XXX | XXX | Dec 2023
| Bare metal, Mariner | E810 VF | Latency | XXX | XXX | Dec 2023
| |  | Message rate | XXX | XXX | Dec 2023
| Bare metal, Ubuntu 20.04 | ConnectX-5 | Latency | XXX | XXX | Dec 2023
| |  | Message rate | XXX | XXX | Dec 2023
| Bare metal, Ubuntu 20.04 | ConnectX-6 DX | Latency | XXX | XXX | Dec 2023
| |  | Message rate | XXX | XXX | Dec 2023




