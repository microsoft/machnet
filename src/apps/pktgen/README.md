# Packet Generator (pktgen)

This is a simple packet generator application. It does not use the Machnet stack.

This packet generator could be used to generate packets with a specific payload size, and send them to a specific destination. It could be used to generate traffic for testing purposes (for example, to test PPS performance of a system).

## Prerequisites

Successful build of the `Machnet` project (see main [README](../../../README.md)).

## Running the application

### Configuration

The configuration is done by editing the [servers.json](../../../servers.json) file. In particular, a new entry needed for each server that runs the `pktgen` application (follow existing examples):

```json
{
    "hosts_config": {
        "zeus": {
            "pcie_allowlist": "607a:00:02.0",
            "pmd_port_id": 1,
            "eal_args": "-c 0x10 -n 4 --vdev=net_vdev_netvsc0,iface=eth1 --log-level=eal,8",
            "mac_addr": "60:45:bd:0f:d7:6e",
            "ipv4_addr": "10.0.255.10"
        },
    }
}
```

The above entry corresponds to interface ```eth1```, which is an Azure vNIC with accelerated networking in VM `zeus`. Note that we need to get the PCIe address of the interface. In Azure VMs, two interfaces hold the same MAC address (synthetic and VF), but only one has a physical PCIe address. To get the PCIe address you may:

```bash
IFACE=eth1 
readlink -f /sys/class/net/${IFACE}/lower* | rev | cut -d '/' -f3 | rev
```


### Running in active mode (packet generator)

When running in this mode the application is actively generating packets. It also receives packets and prints the achieved PPS rate.

From `${REPOROOT}/build/src/apps/pktgen`:

```bash
sudo GLOG_logtostderr=1 ./pktgen --local_hostname zeus --remote_hostname poseidon --active-generator
```

The above command will run the `pktgen` application on machine `zeus`. It will send packets to the `poseidon` machine. The default packet size is 64 bytes; to adjust this append the `--pkt_size` option. For example, to send 1500-byte (max size) packets:

```bash
sudo GLOG_logtostderr=1 ./pktgen --local_hostname zeus --remote_hostname poseidon --active-generator --pkt_size 1500
```

### Running in ping mode (RTT measurement)

When running in this mode the application is actively sending packets to the remote host. The remote host should be running `pktgen` in **bouncing mode** (see subsequent section).

The `pktgen` is collecting roundtrip time measurements. When stopping the appplication (with Ctrl+C), it will print RTT statistics.

From `${REPOROOT}/build/src/apps/pktgen`:
```bash
sudo GLOG_logtostderr=1 ./pktgen --local_hostname poseidon --remote_hostname zeus --ping
```

If you want RTT measurements printed every 1 second, add option `--v=1` to the previous invocation.

To save the samples in a log file add option `--rtt_log path/to/file`.


### Running in passive mode (packet bouncing)

When running in this mode the application is only bouncing packets to the remote host. It does not generate any other packets.


From `${REPOROOT}/build/src/apps/pktgen`:

```bash
sudo GLOG_logtostderr=1 ./pktgen --local_hostname poseidon --remote_hostname zeus
```

The above command will run the `pktgen` application on machine `poseidon`. It will send all received packets to server `zeus`.
