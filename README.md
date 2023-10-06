# Machnet

Machnet is a research prototype, which provides an easy way for applications to
access low-latency userspace networking. Machnet runs as a separate process on
all machines where the application is deployed. Applications interact with
Machnet over shared memory with a familiar socket-like API, and Machnet
processes in the cluster communicate with each other using userspace networking
(DPDK).

Machnet provides the following benefits, in addition to the low latency:

- Works for running on Azure VMs and other cloud environments.
- Multiple applications can simultaneously use the same network interface.
- No need for DPDK expertise, or compiling the application with DPDK.

# Steps to use Machnet on Azure

## 1. Set up two VMs, each with two accelerated NICs

Machnet requires a dedicated NIC on each VM that it runs on. This NIC may be
used by multiple applications that use Machnet.

We recommend the following steps:

  * Creating two VMs on Azure, each with accelerated networking enabled. The VMs will start up with one NIC each, named `eth0`. This NIC is *never* used by Machnet.
  * Shutdown the VMs.
  * Create two new accelerated NICs from the portal, with no public IPs, and add one to each VM. Then restart the VMs. Each VM should now have another NIC named `eth1`, which will be used by Machnet.


## 2. Get the Machnet Docker image

The Machnet binary is provided in the form of a Docker image. Pulling images
from Github container registry requires a few extra steps.

First, generate a Github personal access token for yourself
(https://github.com/settings/tokens) with the read:packages scope. Store it in
the `GITHUB_PAT` environment variable. Then:

```bash
# Reboot like below to allow non-root users to run Docker
sudo groupadd docker && sudo usermod -aG docker $USER && sudo reboot

# We assume that the Github token is stored as GITHUB_PAT
echo ${GITHUB_PAT} | docker login ghcr.io -u <github_username> --password-stdin
docker pull ghcr.io/microsoft/machnet:latest
```

## 3. Start Machnet on both VMs

Using DPDK on Azure requires unbinding the second NIC (`eth1` here ) from the
OS, which will cause this NIC to disappear. **Before this step, note down the IP
and MAC address of the NIC, since we will need them later.** 

```bash
MACHNET_IP_ADDR=`ifconfig eth1 | grep -w inet | tr -s " " | cut -d' ' -f 3`
MACHNET_MAC_ADDR=`ifconfig eth1 | grep -w ether | tr -s " " | cut -d' ' -f 3`

# Unbind eth1 from the OS
sudo modprobe uio_hv_generic
DEV_UUID=$(basename $(readlink /sys/class/net/eth1/device))
sudo driverctl -b vmbus set-override $DEV_UUID uio_hv_generic

# Start Machnet
echo "Machnet IP address: $MACHNET_IP_ADDR, MAC address: $MACHNET_MAC_ADDR"
git clone git@github.com:microsoft/machnet.git
cd machnet
./machnet.sh --mac $MACHNET_MAC_ADDR --ip $MACHNET_IP_ADDR

# Note: If you lose the NIC info, the Azure metadata server has it:
curl -s -H Metadata:true --noproxy "*" "http://169.254.169.254/metadata/instance?api-version=2021-02-01" | jq '.network.interface[1]'
```

## 4. Run the hello_world JavaScript or C# example:

At this point, the Machnet container/process is running on both VMs. We can now
test things end-to-end with a client-server application.

```bash
# Build the Machnet helper library, on both VMs
cd machnet; ./build_shim.sh; cp libmachnet_shim.so js/; cp libmachnet_shim.so csharp/HelloWorld

# On VM A, run JS hello server (requires node)
cd js;
node hello_world.js --local <eth1 IP address of server A>

# On VM B, run C# hello client (requires dotnet-sdk)
cd csharp/HelloWorld
dotnet run --local <eth1 IP address of server B> --remote <eth1 IP address of server A>
```

If everything goes well, VM A should print "Hello World!"

## Application Programming Interface
Applications use the following steps to interact with the Machnet service:

- Initialize the Machnet library using `machnet_init()`.
- In every thread, create a new channel to Machnet using `machnet_attach()`.
- Listen on a port using `machnet_listen()`.
- Connect to remote processes using `machnet_connect()`.
- Send and receive messages using `machnet_send()` and `machnet_recv()`.

See [machnet.h](src/ext/machnet.h) for the full API documentation.

## Developing Machnet

See [INTERNAL.md](INTERNAL.md) for instructions on how to build and test Machnet.
