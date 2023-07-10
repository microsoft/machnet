# Network-Stack-as-a-Service (NSaaS)

This README is a work in progress.

Here, you can find details on how to run the NSaaS service.

## Prerequisites

Successful build of the `NSaaS` project (see main [README](../../../README.md)).


## Running the stack

### Configuration

As this is still work in progress, a significant part of the network configuration is done by editing the [config.json](../pktgen/config.json) file (shared with [pktgen](../pktgen/) and [msg_gen](../msg_gen) applications). In particular, a new entry needed for each server that runs NSaaS (follow existing examples).


### Running

The NSaaS stack is run by the `nsaas` binary. You could see the available options by running `nsaas --help`.

The folowing command will run the NSaaS stack on machine `poseidon`. The stack doesn't initialize any channels or listeners by default. Using the `nsaas_shim` library API  an application will communicate with the controller and create the required channels and listeners on demand.

```bash
cd ${REPOROOT}/build/
sudo GLOG_logtostderr=1 ./src/apps/nsaas/nsaas --local_hostname poseidon
```

To redirect log output to `/tmp`, omit the `GLOG_logtostderr` flag.

The applications that use NSaaS would need to communicate over the relevant shared memory channel. You can find an example of such an application in [msg_gen](../msg_gen/).
