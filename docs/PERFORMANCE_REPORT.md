# Machnet performance report

**Important note: The performance results should be compared across platforms,
since the intra-platform variability (e.g., different pairs of VMs in the same
cloud) is high.**

## Single-connection request-response benchmark

Description: A client sends a request to a server, and the server sends a
response back to the client. The client keeps a configurable number messages in
flight for pipelining.

Start the Machnet Docker container on both client and server

```bash
# Server
./machnet.sh --mac <server_DPDK_MAC> --ip <server_DPDK_IP>

# Client
./machnet.sh --mac <client_DPDK_MAC> --ip <client_DPDK_IP>
```

Run the `msg_gen` benchmark:
```bash
MSG_GEN="docker run -v /var/run/machnet:/var/run/machnet ghcr.io/microsoft/machnet/machnet:latest release_build/src/apps/msg_gen/msg_gen"

# Server
${MSG_GEN} --local_ip <server_DPDK_IP> --msg_size 1024

# Client: Experiment E1: latency
${MSG_GEN} --local_ip <client DPDK IP> --remote_ip <server DPDK IP> --msg_window 1 --tx_msg_size 1024

# Client: Experiment E2: message rate
${MSG_GEN} --local_ip <client DPDK IP> --remote_ip <server DPDK IP> --msg_window 32 --tx_msg_size 1024
```

| Server | NIC, DPDK PMD | Round-trip p50 99 99.9 | RPCs/s | Notes |
| --- | --- | --- | --- | --- |
| Azure F8s_v2 |  CX4-Lx, netvsc | E1: 18 us, 19 us, 25 us | 54K |  No proximity groups
| *Ubuntu 22.03* |  | E2: 41 us, 54 us, 61 us | 753K | 
| | | | | |
| AWS c5.xlarge | ENA, ena | E1: 42 us, 66 us, 105 us | 22K | Proximity groups
| *Amazon Linux* |  | E2: 61 us, 132 us, 209 us | 122K | `--msg_window 8`
| | | | | |
| GCP XXX, XXX | gVNIC | E1: XXX | XXX | | 
| *XXX*|  | E2: XXX | XXX | | 
| | | | | |
| Bare metal | E810 PF, ice | E1: 18 us, 21 us, 22 us | 55K | 
| *Mariner 2.0* |  | E2: 30 us, 33 us, 37 us | 1043K | 
| | | | | |
| Bare metal | E810 VF, iavf | E1: 18 us, 22 us, 22 us | 55K | 
| *Mariner 2.0* |  | E2: 31 us, 35 us, 41 us | 1003K | 
| | | | | |
| Bare metal | Bluefield-2, mlx5 | E1: 9 us, 12 us, 13 us | 99K | 
| *Ubuntu 22.04* |  | E2: 24 us, 26 us, 28 us | 1320K | 
| | | | | |
| Bare metal j| CX5, mlx5 | E1: XXX | XXX | 
| *Ubuntu 20.04* |  | E2: XXX | XXX | 
| | | | | |
| Bare metal | CX6-Dx, mlx5 | E1: XXX | XXX | 
| *Ubuntu 20.04* |  | E2: XXX | XXX | 

