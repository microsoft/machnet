# Packet Drop or Reorder application (cc_perf)

This is a simple packet drop or delay app. It is mainly designed for debugging and developing of congestion control solutions in Machnet.

## Prerequisites

Successful build of the `Machnet` project (see main [README](../../../README.md)).

## Running the application

### Configuration

The `cc_perf` application shares the same configuration file as the Machnet stack. You may check [config.json](../machnet/config.json) for an example.

The `cc_perf` application ignores the `engine_threads` directive in the configuration. Instead, it
uses a single thread for both sending and receiving packets.

**Attention:** When running in Microsoft Azure, the recommended DPDK driver for the accelerated NIC is [`hn_netvsc`](https://doc.dpdk.org/guides/nics/netvsc.html). Check [here](../machnet/README.md#configuration) for instructions on how to bind the NIC to the `uio_hv_generic` driver.
### Running in drop mode

When running in this mode the application is dropping packets. It also receives packets and prints the achieved PPS rate.


```bash
# Drop packets with 10% drop rate between two machnines with .
cd ${REPOROOT}/build/
sudo GLOG_logtostderr=1 ./src/apps/cc_perf/cc_perf -remote_ip $REMOTE_IP -sremote_ip $SECOND_REMOTE_IP -drop_mode -drop_rate 10

# If ran from a different directory, you may need to specify the path to the config file:
sudo GLOG_logtostderr=1 ./cc_perf --config_file ${REPOROOT}/src/apps/machnet/config.json -remote_ip $REMOTE_IP -sremote_ip $SECOND_REMOTE_IP -forward_drop -drop_rate 0.1 

```

### Running in delay mode

When running in this mode the application is delaying packets. It also receives packets and prints the achieved PPS rate.


```bash
# Drop packets with 10% drop rate between two machnines with .
cd ${REPOROOT}/build/
sudo GLOG_logtostderr=1 ./src/apps/cc_perf/cc_perf -remote_ip $REMOTE_IP -sremote_ip $SECOND_REMOTE_IP -delay 10 # in microseconds

# If ran from a different directory, you may need to specify the path to the config file:
sudo GLOG_logtostderr=1 ./cc_perf --config_file ${REPOROOT}/src/apps/machnet/config.json -remote_ip $REMOTE_IP -sremote_ip $SECOND_REMOTE_IP -delay 1