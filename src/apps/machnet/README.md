# Machnet: A high-performance network stack as a sidecar

This README is a work in progress.

Here, you can find details on how to run the Machnet service.

## Prerequisites

Successful build of the `Machnet` project (see main [README](../../../README.md)).


## Running the stack

### Configuration

Machnet controller loads the configuration from a `JSON` file. The configuration
is straightforward. Each entry in the `machnet_config` dictionary corresponds to
a network interface that is managed by Machnet. The key is the MAC address of
the interface, and the value is a dictionary with the following fields:
   * `ip`: the IP address of the interface.
   * `engine_threads`: The number of threads (and NIC HW queues) to use for this interface.
   * `cpu_mask`: The CPU mask to use to affine all engine threads. If not specified, the default is to use all available cores.

**Example [config.json](config.json):**
```json
{
  "machnet_config": {
    "60:45:bd:0f:d7:6e": {
      "ip": "10.0.255.10",
      "engine_threads": 1
    }
  }
}
```

The configuration is shared with other applications (for example,
[msg_gen](../msg_gen/), [pktgen](../pktgen)).

**Attention:** When running in Microsoft Azure, the recommended DPDK driver for the accelerated NIC is [`hn_netvsc`](https://doc.dpdk.org/guides/nics/netvsc.html). To use `NETVSC PMD` all relevant `VMBUS` devices, need to be bound to the userspace I/O driver (`uio_hv_generic`). To do this once, run the following command:
```bash
# Assuming `eth1` is the interface to be used by Machnet:
DEV_UUID=$(basename $(readlink /sys/class/net/eth1/device))
driverctl -b vmbus set-override $DEV_UUID uio_hv_generic
```


### Running

The Machnet stack is run by the `machnet` binary. You could see the available options by running `machnet --help`.

The folowing command will run the Machnet stack. The stack doesn't initialize any channels or listeners by default. Those are created and destroyed on demand by the applications that use Machnet. The `machnet` binary will run in the foreground, and will print logs to `stderr` if the `GLOG_logtostderr` option is set.

```bash
cd ${REPOROOT}/build/
sudo GLOG_logtostderr=1 ./src/apps/machnet/machnet

# If ran from a different directory, you may need to specify the path to the config file:
sudo GLOG_logtostderr=1 ./some/path/machnet --config_file ${REPOROOT}/src/apps/machnet/config.json
```

You should be able to `ping` Machnet from a remote machine in the same subnet.

To redirect log output to a file in `/tmp`, omit the `GLOG_logtostderr` option.

You can find an example of an application that uses the Machnet stack in [msg_gen](../msg_gen/).
