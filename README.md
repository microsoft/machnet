# NSaaS (Network Stack as a Service)

# Getting started

NSaaS provides an easy way for applications to access low-latency userspace
networking. NSaaS runs as a separate process on all machines where the
application is deployed. Applications interact with NSaaS over shared memory
with a familiar socket-like API, and NSaaS processes in the cluster communicate
with each other using userspace networking (DPDK).

NSaaS provides the following benefits, in addition to the low-latency:

- First-class support for running on Azure VMs and other cloud environments.
- Multiple applications can simultaneously use the same network interface.
- No need for DPDK expertise, or compiling the application with DPDK.

## Get the NSaaS binary

The NSaaS binary is provided in the form of a Docker image. To get this, you
will need (1) access to the MSR-NSaaS repo, (2) a Github personal access
token (https://github.com/settings/tokens) with the read:packages scope, and (3)
ask an admin to give you access at
[link](https://github.com/orgs/MSR-NSaaS/packages/container/nsaas/settings)

Start NSaaS on two servers, A & B. Below, we assume that the accelerated DPDK-capable NIC is named `eth1`:

```bash
# Reboot like below to allow non-root users to run Docker
sudo groupadd docker && sudo usermod -aG docker $USER && sudo reboot

# We assume that the Github token is stored as GITHUB_PAT
echo ${GITHUB_PAT} | docker login ghcr.io -u <github_username> --password-stdin
docker pull ghcr.io/msr-nsaas/nsaas:latest

# Start NSaaS on the accelerated NIC (e.g., eth1 on Azure)
git clone git@github.com:MSR-NSaaS/nsaas.git
cd nsaas
./nsaas.sh --mac <MAC address of eth1> --ip <IP address of eth1>
```

## Run the hello_world JavaScript or C# example:

```bash
# Build the NSaaS helper library
cd nsaas; ./build_shim.sh; cp libnsaas_shim.so js/; cp libnsaas_shim.so csharp/HelloWorld

# On server A, run JS hello server (requires node)
cd js;
node hello_world.js --local <eth1 IP address of server A>

# On server B, run C# hello client (requires dotnet-sdk)
cd csharp/HelloWorld
dotnet run --local <eth1 IP address of server B> --remote <eth1 IP address of server A>
```

If everything goes well, server A should print "Hello World!"

## Application Programming Interface
Applications use the following steps to interact with the NSaaS service:

- Initialize the NSaaS library using `nsaas_init()`.
- In every thread, create a new channel to NSaaS using `nsaas_attach()`.
- Listen on a port using `nsaas_listen()`.
- Connect to remote processes using `nsaas_connect()`.
- Send and receive messages using `nsaas_send()` and `nsaas_recv()`.

See `src/ext/nsaas.h` for the full API documentation.

## Developing NSaaS

See `INTERNAL.md` for instructions on how to build and test NSaaS.
