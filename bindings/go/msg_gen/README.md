# Mesage Generator Go App (main)

This is a simple message generator application that uses the Machnet stack.

## Prerequisites

Successful build of the `Machnet` project (see main [README](../../README.md)).


## Running the application

### Configuration

Install the necessary go dependencies by way of:

```
cd apps/msg_gen
go install
```

Build the go binary using:

```
go build main.go
```

The application is run by the `main` binary. You could see the available options by running `main --help`.

### Sending messages between two machines

**Attention**: An Machnet stack instance must be running on every machine that needs to use this message generator application. You can find information on how to run the Machnet stack in the [Machnet README](../../src/apps/machnet/README.md).

In the example below, a server named `poseidon` sends messages to the `zeus` server, which bounces them back.

```bash
# On machine `zeus` (bouncing):
cd ${REPOROOT}/src/apps/main
sudo GLOG_logtostderr=1 ./main --local_hostname zeus

# On machine `poseidon` (sender):
cd ${REPOROOT}/src/apps/main
sudo GLOG_logtostderr=1 ./main --local_hostname poseidon --remote_hostname zeus --msg_size 20000 -active_generator
```

The active generator side will maintain a closed loop with a pre-set message window (that is, number of inflight messages). You can adjust the number of inflight messages by setting the `-msg_window` option. The default value is 8.

### Options for Message Generator Application
1. `msg_size`: Set the size of the message to test against.
2. `msg_window`: Set the maximum number of messages in flight.
3. `active_generator`: If set, the application actively sends messages and reports the stats.
4. `latency`: Get the latency measurements. Default: `false` (gives throughput measurements in that case)

For all options, run `./main --help`
