# Faster workload generator (faster_tcp)
In this directory we would like to benchmark three different transports using our key-value store. We have got three transport, UDP, TCP and Machnet.

## Prerequisites

Successful build of the `Machnet` project good to have (see main [README](../../../README.md)). Please check the build files for this.

## Running the applications

### TCP Transport

The `faster_tcp` application is run by the `faster_tcp` binary. You could see the
available options by running `faster_tcp --help`.

### UDP Transport and Machnet Transport
The `faster_client` application is run by the `faster_client` binary. You could see the
available options by running `faster_client --help`. This binary supports both UDP and Machnet.

### Sending messages between two machines for all three cases.

#### machnet

In the example below, a server with IP `10.0.0.1` sends messages to the remote
server with IP `10.0.0.2`, which bounces them back.

```bash
# On machine `10.0.0.2` (bouncing):
cd ${REPOROOT}/build/src/apps/faster
sudo GLOG_logtostderr=1 ./faster_server -transport machnet -local 10.0.0.2 -num_keys 10000
# GLOG_logtostderr=1 can be omitted if you want to log to a file instead of stderr.

# On machine `10.0.0.1` (sender):
cd ${REPOROOT}/build/src/app/faster
sudo GLOG_logtostderr=1 ./faster_client -active_generator -transport machnet -local_ip 10.0.0.2 --remote_ip 10.0.0.1

```
#### UDP

In the example below, a server with IP `10.0.0.1` sends messages to the remote
server with IP `10.0.0.2`, which bounces them back.

```bash
# On machine `10.0.0.2` (bouncing):
cd ${REPOROOT}/build/src/apps/faster
sudo GLOG_logtostderr=1 ./faster_server -transport UDP -local 10.0.0.2 -num_keys 10000
# GLOG_logtostderr=1 can be omitted if you want to log to a file instead of stderr.

# On machine `10.0.0.1` (sender):
cd ${REPOROOT}/build/src/app/faster
sudo GLOG_logtostderr=1 ./faster_client -active_generator -transport UDP -local_ip 10.0.0.2 --remote_ip 10.0.0.1
```
#### TCP

In the example below, a server with IP `10.0.0.1` sends messages to the remote
server with IP `10.0.0.2`, which bounces them back.

```bash
# On machine `10.0.0.2` (bouncing):
cd ${REPOROOT}/build/src/apps/faster
sudo GLOG_logtostderr=1 ./faster_tcp -remote-ip 10.0.0.1  -port 10000
# GLOG_logtostderr=1 can be omitted if you want to log to a file instead of stderr.

# On machine `10.0.0.1` (sender):
cd ${REPOROOT}/build/src/app/faster
sudo GLOG_logtostderr=1 ./faster_tcp -active_generator --remote_ip 10.0.0.2 -port 10000
```
