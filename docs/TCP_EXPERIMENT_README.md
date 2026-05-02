# Running TCP Experiments with Machnet

This guide shows how to run TCP-based applications with Machnet. The key idea:
**a standard Linux TCP client (using POSIX sockets) can talk to a Machnet
server running a DPDK kernel-bypass TCP stack** — getting sub-100μs tail
latency on the server side without any changes to the client.

## Wire Protocol

Machnet TCP uses a simple framing format. Every message on the wire is:

```
[4-byte big-endian length prefix][payload bytes]
```

Any client that sends/receives with this framing can interoperate.

**Prerequisites:** Two machines with Machnet already running on at least one of
them. Follow the main [README.md](../README.md) for NIC setup and starting the
Machnet process. The client VM needs nothing special — any Linux machine works.

---

## Experiment 1: Machnet Server + Standard TCP Client

The server uses DPDK kernel-bypass, the client uses regular Linux sockets.

### Run the Machnet TCP server (msg_gen)

On the server VM (Machnet process already running per the main README):

```bash
MSG_GEN="docker run -v /var/run/machnet:/var/run/machnet \
  ghcr.io/microsoft/machnet/machnet:latest \
  release_build/src/apps/msg_gen/msg_gen"

# Start TCP server listening on port 888
${MSG_GEN} --local_ip ${MACHNET_IP} --local_port 888 --protocol tcp --msg_size 64
```

You should see:
```
Using TCP transport
Starting in server mode, response size 64
[LISTENING] [10.0.0.2:888]
```

### Run the standard TCP client (tcp_msg_gen)

On the **client VM** (no Machnet needed):

```bash
# Build tcp_msg_gen — pure POSIX sockets, no DPDK dependency
cd machnet/build
cmake .. && ninja -j$(nproc) tcp_msg_gen

# Connect to the Machnet server
./src/apps/tcp_msg_gen/tcp_msg_gen \
  --local_ip=0.0.0.0 \
  --remote_ip=<MACHNET_SERVER_IP> \
  --remote_port=888 \
  --msg_size=64 \
  --msg_window=8
```

You should see throughput stats printed every second:
```
[CLIENT] Connecting to 10.0.0.2:888, msg_size=64, window=8
[CLIENT] Connected.
TX/RX (msg/sec, Gbps): (142.3K/142.3K, 0.073/0.073)
TX/RX (msg/sec, Gbps): (145.1K/145.1K, 0.074/0.074)
```

### (Optional) Use a Python client instead

You can also connect from any language. Here's a minimal Python client:

```python
#!/usr/bin/env python3
"""Minimal TCP client that speaks Machnet's wire protocol."""
import socket, struct, time

SERVER_IP = "<MACHNET_SERVER_IP>"
SERVER_PORT = 888
MSG_SIZE = 64

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
sock.connect((SERVER_IP, SERVER_PORT))

payload = bytes(MSG_SIZE)
count = 0
t0 = time.time()

while True:
    # Send: 4-byte BE length prefix + payload
    sock.sendall(struct.pack('>I', len(payload)) + payload)

    # Recv: read 4-byte prefix, then payload
    hdr = sock.recv(4, socket.MSG_WAITALL)
    if len(hdr) < 4:
        break
    msg_len = struct.unpack('>I', hdr)[0]
    data = sock.recv(msg_len, socket.MSG_WAITALL)

    count += 1
    elapsed = time.time() - t0
    if elapsed >= 1.0:
        print(f"{count/elapsed:.0f} msg/sec")
        count = 0
        t0 = time.time()
```

---

## Experiment 2: Machnet-to-Machnet TCP

Both sides run Machnet (kernel-bypass on both endpoints).

### Server VM

```bash
${MSG_GEN} --local_ip ${MACHNET_IP} --local_port 888 --protocol tcp --msg_size 64
```

### Client VM (Machnet also running)

```bash
${MSG_GEN} --local_ip ${CLIENT_IP} \
           --remote_ip ${SERVER_IP} \
           --remote_port 888 \
           --protocol tcp \
           --msg_size 64 \
           --msg_window 8
```

---

## Experiment 3: Standard TCP Server + Machnet Client

A plain Linux TCP server paired with a Machnet kernel-bypass client.

### Server VM (no Machnet needed)

```bash
# Build tcp_msg_gen (same build as Experiment 1 client)
./src/apps/tcp_msg_gen/tcp_msg_gen \
  --local_ip=0.0.0.0 \
  --local_port=888 \
  --msg_size=64
```

### Client VM (running Machnet)

```bash
${MSG_GEN} --local_ip ${MACHNET_IP} \
           --remote_ip <SERVER_IP> \
           --remote_port 888 \
           --protocol tcp \
           --msg_size 64 \
           --msg_window 8
```

---

## Experiment Summary

| Experiment | Client | Server | What it shows |
|---|---|---|---|
| 1 | `tcp_msg_gen` (POSIX sockets) | `msg_gen --protocol tcp` (Machnet) | Standard TCP client → kernel-bypass server |
| 2 | `msg_gen --protocol tcp` (Machnet) | `msg_gen --protocol tcp` (Machnet) | Full kernel-bypass on both sides |
| 3 | `msg_gen --protocol tcp` (Machnet) | `tcp_msg_gen` (POSIX sockets) | Kernel-bypass client → standard TCP server |

---

## Configuration Reference

### msg_gen flags (Machnet TCP)

| Flag | Default | Description |
|---|---|---|
| `--local_ip` | (required) | IP of the local Machnet interface |
| `--remote_ip` | (empty = server) | IP of the remote peer (set to run as client) |
| `--local_port` | 888 | Local port to listen on |
| `--remote_port` | 888 | Remote port to connect to |
| `--protocol` | `udp` | Transport protocol: `udp` or `tcp` |
| `--msg_size` | 64 | Message payload size in bytes (minimum 9) |
| `--msg_window` | 8 | Number of messages in flight (client only) |
| `--msg_nr` | unlimited | Total number of messages to send |
| `--verify` | false | Verify payload correctness |

### tcp_msg_gen flags (standard POSIX TCP)

| Flag | Default | Description |
|---|---|---|
| `--local_ip` | `0.0.0.0` | Local bind address |
| `--local_port` | 888 | Local port (server mode) |
| `--remote_ip` | (empty = server) | Remote address (set to run as client) |
| `--remote_port` | 888 | Remote port (client mode) |
| `--msg_size` | 64 | Message payload size in bytes (minimum 8) |
| `--msg_window` | 8 | Number of messages in flight (client only) |

---

## Troubleshooting

**`tcp_msg_gen` fails to connect:**
- Verify the Machnet server is running and listening (`[LISTENING]` in the log).
- Check firewall rules allow traffic on port 888 between the two VMs.
- Ensure the Machnet NIC is bound to DPDK (it won't respond to `ping`).

**Low throughput:**
- Increase `--msg_window` for more pipelining (try 16, 32, 64).
- Increase `--msg_size` if your workload has larger messages.
- Ensure `TCP_NODELAY` is enabled on the client (both `tcp_msg_gen` and
  the Python example do this).

**Connection reset / drops:**
- Check that both sides use the same `--msg_size` — the app header
  (`window_slot`) must be present in every message.
- Ensure hugepages are configured on the Machnet VM
  (`echo 2048 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages`).
