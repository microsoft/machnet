This document is for developers of the Machnet service. It is not intended for
users of the Machnet service. For users, see the [README](README.md).

## Compiling Machnet as a Docker image

### Building the standard x86 image

```bash
docker build --no-cache -f dockerfiles/ubuntu-22.04.dockerfile --target machnet --tag machnet .
```

### Building custom images

The top-level makefile allows building x86 and arm64 docker images across a 
variety of microarchitecture capability levels (x86) or SOC targets (arm64). 
Invoking "make" will create them for the architecture of the host. 

To build a Machnet image for a specific architecture, e.g, x86-64-v4

```bash
# This requires a recent Docker version
$ docker buildx bake -f docker-bake x86-64-v4
```

## Manually compiling Machnet from source

Install required packages
```bash
$ sudo apt -y install cmake pkg-config nlohmann-json3-dev ninja-build gcc g++ doxygen graphviz python3-pip meson libhugetlbfs-dev libnl-3-dev libnl-route-3-dev uuid-dev
$ pip3 install pyelftools
```

For Ubuntu 20.04 we need at least versions `gcc-10`, `g++-10`, `cpp-10`. This step is not required for newer versions of Ubuntu.
```bash
# Install and set gcc-10 and g++-10 as the default compiler.
$ sudo apt install gcc-10 g++-10
$ sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-10 100 --slave /usr/bin/g++ g++ /usr/bin/g++-10 --slave /usr/bin/gcov gcov /usr/bin/gcov-10
```

### Build and install rdma-core

```bash
# Remove conflicting packages
$ RUN apt-get --purge -y remove rdma-core librdmacm1 ibverbs-providers libibverbs-dev libibverbs1
```

Now we install rdma-core from source:
```bash
export RDMA_CORE=/path/to/rdma-core
git clone -b 'stable-v40' --single-branch --depth 1 https://github.com/linux-rdma/rdma-core.git ${RDMA_CORE}
cd ${RDMA_CORE}
mkdir -p build && cd build
cmake -GNinja -DNO_PYVERBS=1 -DNO_MAN_PAGES=1 ..
ninja install # as root
ldconfig
```

### Build DPDK

Download and extract DPDK 23.11, then:

```bash
cd dpdk-23.11
meson build --prefix=${PWD}/build/install/usr/local
ninja -C build
ninja -C build install
export PKG_CONFIG_PATH="${PWD}/build/install/usr/local/lib/x86_64-linux-gnu/pkgconfig:${PWD}/build/install/usr/local/lib/pkgconfig"
```

Note: You can find the PKG_CONFIG_PATH by the following command: find *path_to_dpdk* -name "*.pc".

### Build Machnet

Then, from `${REPOROOT}`:
```bash
git submodule update --init --recursive

# Debug build:
mkdir build && cd build && cmake -DCMAKE_BUILD_TYPE=Debug -GNinja ../ && ninja

# Release build:
mkdir build && cd build && cmake -DCMAKE_BUILD_TYPE=Release -GNinja ../ && ninja
```

The `machnet` binary will be available in `${REPOROOT}/build/src/apps/`.  You may
see more details about the `Machnet` program in this
[README](src/apps/machnet/README.md).

## Tests

To run all tests, from ${REPOROOT}:
```bash
# If the Ansible automation was not used, enable hugepages first
echo 1024 | sudo tee /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages
sudo ctest # sudo is required for DPDK-related tests.
```
