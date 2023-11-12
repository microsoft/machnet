This document is for developers of the Machnet service. It is not intended for
users of the Machnet service. For users, see the [README](README.md).

## Compiling Machnet

[Dockerfile](Dockerfile) is the reference for the required dependencies and the
overall build process. The following instructions follow the same steps as the
Dockerfile.


```bash
# On latest Ubuntu:
sudo apt -y install cmake libgflags-dev pkg-config nlohmann-json3-dev ninja-build gcc-10 g++-10 doxygen graphviz python3-pip meson libhugetlbfs-dev libnl-3-dev libnl-route-3-dev
pip3 install pyelftools
```


  For Ubuntu 20.04 we need at least versions `gcc-10`, `g++-10`, `cpp-10`. This step is not required for newer versions of Ubuntu.
  ```bash
  # Set gcc-10 and g++-10 as the default compiler.
  update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-10 100 --slave /usr/bin/g++ g++ /usr/bin/g++-10 --slave /usr/bin/gcov gcov /usr/bin/gcov-10
  ```

### Build and install rdma-core

```bash
# Remove conflicting packages
RUN apt-get --purge -y remove rdma-core librdmacm1 ibverbs-providers libibverbs-dev libibverbs1
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

We use DPDK v21.11 LTS, like so:

```bash
export RTE_SDK=/path/to/dpdk-21.11/
cd ${RTE_SDK} && meson build && cd build && ninja && DESTDIR=${PWD}/install ninja install
```

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

## Docker
```bash
# Build the docker image (from the root of the repository):
docker build --no-cache -f Dockerfile --target  machnet .
```

## Tests

To run all tests, from ${REPOROOT}:
```bash
# If the Ansible automation was not used, enable hugepages first
echo 1024 | sudo tee /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages
sudo ctest # sudo is required for DPDK-related tests.
```
