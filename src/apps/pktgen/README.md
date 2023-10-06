# Packet Generator (pktgen)

This is a simple packet generator application. It does not provide a full-fledged network stack, but rather uses raw datagrams with DPDK. Suitable for testing baseline performance of a system.

Among others, the application can be used for two purposes:
 * Load generator: actively generates packets (configurable sizes) and sends them to a remote host. It also receives packets and prints the achieved PPS rate.
 * RTT measurement: actively sends packets to a remote host. The remote host should be running `pktgen` in **bouncing mode** (see subsequent section). The `pktgen` is collecting roundtrip time measurements. When stopping the appplication (with Ctrl+C), it will print RTT statistics.


## Prerequisites

Successful build of the `Machnet` project (see main [README](../../../README.md)).

## Running the application

### Configuration

The `pktgen` application shares the same configuration file as the Machnet stack. You may check [config.json](../machnet/config.json) for an example.

The `pktgen` application ignores the `engine_threads` directive in the configuration. Instead, it
uses a single thread for both sending and receiving packets.

**Attention:** When running in Microsoft Azure, the recommended DPDK driver for the accelerated NIC is [`hn_netvsc`](https://doc.dpdk.org/guides/nics/netvsc.html). Check [here](../machnet/README.md#configuration) for instructions on how to bind the NIC to the `uio_hv_generic` driver.
### Running in active mode (packet generator)

When running in this mode the application is actively generating packets. It also receives packets and prints the achieved PPS rate.


```bash
# Send packets to a remote host.
REMOTE_IP="10.0.0.254"
cd ${REPOROOT}/build/
sudo GLOG_logtostderr=1 ./src/apps/pktgen/pktgen --remote_ip $REMOTE_IP --active-generator

# If ran from a different directory, you may need to specify the path to the config file:
sudo GLOG_logtostderr=1 ./pktgen --config_file ${REPOROOT}/src/apps/machnet/config.json --remote_ip $REMOTE_IP --active-generator

```

The above command will run the `pktgen` application to a remote machine with IP `10.0.0.254` in the same subnet. The default packet size is 64 bytes; to adjust this append the `--pkt_size` option. For example, to send 1500-byte (max size) packets:

```bash
# From ${REPOROOT}/build/
sudo GLOG_logtostderr=1 ./src/apps/pktgen/pktgen --remote_ip $REMOTE_IP --active-generator --pkt_size 1500
```

### Running in ping mode (RTT measurement)

When running in this mode the application is actively sending packets to the remote host. The remote host should be running `pktgen` in **bouncing mode** (see subsequent section).

The `pktgen` is collecting roundtrip time measurements. When stopping the appplication (with Ctrl+C), it will print RTT statistics.

```bash
REMOTE_IP="10.0.0.254"
# From ${REPOROOT}/build/
sudo GLOG_logtostderr=1 ./src/apps/pktgen/pktgen --remote_ip $REMOTE_IP --ping
```

If you want RTT measurements printed every 1 second, add option `--v=1` to the previous invocation.

To save the samples in a log file add option: `--rtt_log path/to/file`.


### Running in passive mode (packet bouncing)

When running in this mode the application is only bouncing packets to the remote host. It does not generate any other packets.


From `${REPOROOT}/build/src/apps/pktgen`:

```bash
REMOTE_IP="10.0.0.254"
# From ${REPOROOT}/build/
sudo GLOG_logtostderr=1 ./src/apps/pktgen/pktgen
```

The above command will run the `pktgen` application in **passive** mode on local machine. It will send all received packets to the originating remote server.
