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

## Get the Machnet binary

The Machnet binary is provided in the form of a Docker image. To get this, you
will need (1) access to the Machnet repository, (2) a Github personal access
token (https://github.com/settings/tokens) with the read:packages scope, and (3)
ask an admin to give you access.

Start Machnet on two servers, A & B. Below, we assume that the accelerated DPDK-capable NIC is named `eth1`:

```bash
# Reboot like below to allow non-root users to run Docker
sudo groupadd docker && sudo usermod -aG docker $USER && sudo reboot

# We assume that the Github token is stored as GITHUB_PAT
echo ${GITHUB_PAT} | docker login ghcr.io -u <github_username> --password-stdin
docker pull ghcr.io/microsoft/machnet:latest

# Start Machnet on the accelerated NIC (for example, eth1 on Azure)
git clone git@github.com:microsoft/machnet.git
cd machnet
./machnet.sh --mac <MAC address of eth1> --ip <IP address of eth1>
```

## Run the hello_world JavaScript or C# example:

```bash
# Build the Machnet helper library
cd machnet; ./build_shim.sh; cp libmachnet_shim.so js/; cp libmachnet_shim.so csharp/HelloWorld

# On server A, run JS hello server (requires node)
cd js;
node hello_world.js --local <eth1 IP address of server A>

# On server B, run C# hello client (requires dotnet-sdk)
cd csharp/HelloWorld
dotnet run --local <eth1 IP address of server B> --remote <eth1 IP address of server A>
```

If everything goes well, server A should print "Hello World!"

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
