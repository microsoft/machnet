# Ping utility

This is a simple DPDK-based ping utility. It can be used to measure roundtrip
time (RTT) between two machines.


## Prerequisites

Successful build of the `Machnet` project (see main [README](../../../README.md)).

## Running the application

### Configuration

The configuration is done by editing the [config.json](../machnet/config.json) file similar to the [machnet](../machnet) application. The configuration file should contain exactly one interface configuration.

```json
{
  "machnet_config": {
    "00:0d:3a:d6:9b:6e": {
      "ip": "10.0.255.254",
      "cpu_mask": "0xffff"
    }
  }
}
```

The above entry corresponds to the MAC address of some active network interface, and the IP address and CPU mask to be used by the application. The CPU mask is a hexadecimal number that specifies the cores that the application will use. For example, `0xffff` means that the application will use all cores. If the CPU mask is not specified, the application will use all cores.

Other legal configuration options are safely ignored (for example, `engine_threads`).

**Attention:** The ping utility is going to take over the specified interface. This means that any other application that is using the same interface will stop working!

### Running

From `${REPOROOT}/build/src/apps/ping`:

```bash
sudo GLOG_logtostderr=1 ./ping --remote_ip 10.0.0.1
```
**Attention**: Currently the utility expects the remote host to be specified with its IP address. This will be fixed in the future to support hostnames as well.
