# Machnet: Easy kernel-bypass messaging between cloud VMs

[![Build](https://github.com/microsoft/machnet/actions/workflows/build.yml/badge.svg?event=push)](https://github.com/microsoft/machnet)

Machnet provides an easy way for applications to reduce their datacenter
networking latency via kernel-bypass (DPDK-based) messaging. Distributed
applications like databases and finance can use Machnet as the networking
library to get sub-100 microsecond tail latency at high message rates, e.g.,
**750,000 1KB request-reply messages per second on Azure F8s_v2 VMs with 61
microsecond P99.9 round-trip latency**. We support a variety of cloud (Azure,
AWS, GCP) and bare-metal platforms, OSs and NICs, evaluated in
[docs/PERFORMANCE_REPORT.md](docs/PERFORMANCE_REPORT.md).

While there are several other DPDK-based network stacks, Machnet provides the
following unique benefits:

- Specifically designed for and tested on public cloud VM environments
- Multiple applications on the same machine can use Machnet
- No need for DPDK expertise, or compiling the application with DPDK

**Architecture**: Machnet runs as a separate process on all machines where the
application is deployed and mediates access to the DPDK NIC. Applications
interact with Machnet over shared memory with a sockets-like API. Machnet
processes in the cluster communicate with each other using DPDK.

# Steps to use Machnet

## 1. Set up two machines with two NICs each

Machnet requires a dedicated NIC on each machine that it runs on. This NIC may be
used by multiple applications that use Machnet.

On Azure, we recommend the following steps:

  1. Create two VMs with accelerated networking enabled. The VMs will start up with one NIC each, named `eth0`. This NIC is *never* used by Machnet.
  2. Shut-down the VMs.
  3. Create two new accelerated NICs from the portal, with no public IPs, and add one to each VM.
  4. After restarting, each VM should have another NIC named `eth1`, which will be used by Machnet.

The `examples` directory contains detailed scripts/instructions to launch VMs for Machnet.

## 2. Get the Docker image with a pre-built Machnet

You can either build the Docker image using the Dockerfile in this repo, or pull
it from Github container registry. Pulling from GHCR requires an auth token:

 1. Generate a Github personal access token for yourself (https://github.com/settings/tokens) with the read:packages scope. and store it in the `GITHUB_PAT` environment variable.
 2. At `https://github.com/settings/tokens`, follow the steps to "Configure SSO" for this token.

```bash
# Install packages required to try out Machnet
sudo apt-get update
sudo apt-get install -y docker.io net-tools driverctl libgflags-dev uuid-dev

# Reboot like below to allow non-root users to run Docker
sudo usermod -aG docker $USER && sudo reboot

# We assume that the Github token is stored as GITHUB_PAT
echo ${GITHUB_PAT} | docker login ghcr.io -u <github_username> --password-stdin
docker pull ghcr.io/microsoft/machnet/machnet:latest
```

## 3. Start the Machnet process on both VMs

Using DPDK often requires unbinding the dedicated NIC from the OS. This will
cause the NIC to disappear from tools like `ifconfig`. **Before this step,
note down the IP and MAC address of the NIC, since we will need them
later.**

Below, we assume that the dedicated NIC is named `eth1`.  These steps can be
automated using a script like
[azure_start_machnet.sh](examples/azure_start_machnet.sh) that uses the
cloud's metadata service to get the NIC's IP and MAC address.

```bash
MACHNET_IP_ADDR=`ifconfig eth1 | grep -w inet | tr -s " " | cut -d' ' -f 3`
MACHNET_MAC_ADDR=`ifconfig eth1 | grep -w ether | tr -s " " | cut -d' ' -f 3`

# If on Azure, use driverctl to unbind the NIC instead of dpdk-devbind.py:
sudo modprobe uio_hv_generic
DEV_UUID=$(basename $(readlink /sys/class/net/eth1/device))
sudo driverctl -b vmbus set-override $DEV_UUID uio_hv_generic

# Otherwise, use dpdk-devbind.py like so
# sudo <dpdk_dir>/usertools/dpdk-devbind.py --bind=vfio-pci <PCIe address of dedicated NIC>

# Start Machnet
echo "Machnet IP address: $MACHNET_IP_ADDR, MAC address: $MACHNET_MAC_ADDR"
git clone --recursive https://github.com/microsoft/machnet.git
cd machnet
./machnet.sh --mac $MACHNET_MAC_ADDR --ip $MACHNET_IP_ADDR

# Note: If you lose the NIC info, the Azure metadata server has it:
curl -s -H Metadata:true --noproxy "*" "http://169.254.169.254/metadata/instance?api-version=2021-02-01" | jq '.network.interface[1]'
```

## 4. Run the hello world example

At this point, the Machnet container/process is running on both VMs. We can now
test things end-to-end with a client-server application.

```bash
# Build the Machnet helper library and hello_world example, on both VMs
./build_shim.sh
cd hello_world; make

# On VM #1, run the hello_world server
./hello_world --local <eth1 IP address of VM 1>

# On VM #2, run the hello_world client. This should print the reply from the server.
./hello_world --local <eth1 IP address of VM 1> --remote <eth1 IP address of VM 2>
```

## 5. Run the end-to-end benchmark

The Docker image contains a pre-built benchmark called `msg_gen`.
```bash
MSG_GEN="docker run -v /var/run/machnet:/var/run/machnet ghcr.io/microsoft/machnet/machnet:latest release_build/src/apps/msg_gen/msg_gen"

# On VM #1, run the msg_gen server
${MSG_GEN} --local_ip <eth1 IP address of VM 1>

# On VM #2, run the msg_gen client
${MSG_GEN} --local_ip <eth1 IP address of VM 1> --remote_ip <eth1 IP address of VM 2>
```

The client should print message rate and latency percentile statistics.
`msg_gen --help` lists all the options available.

We can also build `msg_gen` from source without DPDK or rdma_core:
```bash
cd machnet
rm -rf build; mkdir build; cd build; cmake -DCMAKE_BUILD_TYPE=Release ..; make -j
MSG_GEN="~/machnet/build/src/apps/msg_gen/msg_gen"
```



## Machnet API

See [machnet.h](src/ext/machnet.h) for the full API documentation.  Applications use the following steps to interact with the Machnet service:

- Initialize the Machnet library using `machnet_init()`.
- In every thread, create a new shared-memory channel to Machnet using `machnet_attach()`.
- Listen on a port using `machnet_listen()`.
- Connect to remote processes using `machnet_connect()`.
- Send and receive messages using `machnet_send()` and `machnet_recv()`.


## Developing Machnet

See [CONTRIBUTING.md](CONTRIBUTING.md). for instructions on how to build and test Machnet.
