# Message Generator (msg_gen)

This is a simple message generator application that uses the Machnet stack.

## Prerequisites

Successful build of the `Machnet` project (see main [README](../../../README.md)).

A `Machnet` stack instance must already be running on the machine that needs to use this
message generator application. You can find information on how to run the
`Machnet` stack in the [README](../machnet/README.md).


## Running the application

The `msg_gen` application is run by the `msg_gen` binary. You could see the
available options by running `msg_gen --help`.

### Sending messages between two machines

In the example below, a server with IP `10.0.0.1` sends messages to the remote
server with IP `10.0.0.2`, which bounces them back.

```bash
# On machine `10.0.0.2` (bouncing):
cd ${REPOROOT}/build/
sudo GLOG_logtostderr=1 ./src/apps/msg_gen/msg_gen --local_ip 10.0.0.2
# GLOG_logtostderr=1 can be omitted if you want to log to a file instead of stderr.

# On machine `10.0.0.1` (sender):
cd ${REPOROOT}/build/
sudo GLOG_logtostderr=1 ./src/apps/msg_gen/msg_gen --local_ip 10.0.0.2 --remote_ip 10.0.0.1

```
