# Mesage Generator (msg_gen)

This is a simple message generator application that uses the Machnet stack.

## Prerequisites

Successful build of the `Machnet` project (see main [README](../../../README.md)).


## Running the application

### Configuration

This application shares main network configuration with the [pktgen](../pktgen) application. The configuration is done by editing the [servers.json](../../../servers.json) file. In particular, a new entry needed for each server that runs the `msg_gen` application (follow existing examples). Please refer to the `pktgen` [README](../pktgen/README.md) for information on how to obtain the PCIe address info.

The `msg_gen` application is run by the `msg_gen` binary. You could see the available options by running `msg_gen --help`.

### Sending messages between two machines

**Attention**: An Machnet stack instance must be running on every machine that needs to use this message generator application. You can find information on how to run the Machnet stack in the [Machnet README](../machnet/README.md).

In the example below, a server named `poseidon` sends messages to the `zeus` server, which bounces them back.

```bash
# On machine `poseidon` (sender):
cd ${REPOROOT}/build/
sudo GLOG_logtostderr=1 ./src/apps/msg_gen/msg_gen --local_hostname poseidon --remote_hostname zeus --channel test_channel --msg_size 20000 --active-generator

# On machine `zeus` (bouncing):
cd ${REPOROOT}/build/
sudo GLOG_logtostderr=1 ./src/apps/msg_gen/msg_gen --local_hostname zeus
```

The active generator side will maintain a closed loop with a pre-set message window (that is, number of inflight messages). You can adjust the number of inflight messages by setting the `--msg_window` option. The default value is 8.

To verify the payload of received messages, add the option `--verify` on the machine acting as active generator.
